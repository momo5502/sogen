#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <vector>
#include <ranges>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t buffer_base = 0x300000000ULL;
    constexpr uint64_t carry = 0x20000000ULL;
    constexpr uint8_t untouched = 0xAB;

    size_t distinct_bytes(const uint8_t* data, const size_t size)
    {
        std::vector<uint8_t> values(data, data + size);
        std::ranges::sort(values);
        const auto duplicates = std::ranges::unique(values);
        return static_cast<size_t>(std::distance(values.begin(), duplicates.begin()));
    }

    void run_syscall(sogen::macos_emulator& emu, const uint32_t mov_x16)
    {
        macos_test::write_guest_code(emu, code_base,
                                     {
                                         mov_x16,
                                         0xD4001001, // svc #0x80
                                     });
        emu.start(2);
    }

    // The backend caches translated blocks, so a second syscall in one emulator has to be assembled at a
    // fresh address; rewriting code_base would replay the first block instead.
    void run_syscall_at(sogen::macos_emulator& emu, const uint64_t address, const uint32_t mov_x16)
    {
        const std::array<uint32_t, 2> words{mov_x16, 0xD4001001};
        emu.memory.write_memory(address, words.data(), sizeof(words));
        emu.emu().reg(sogen::arm64_register::pc, address);
        emu.start(2);
    }

    // bsdthread_create does not start a thread at the routine the caller named. libpthread registers a
    // trampoline first and the kernel starts every thread there, passing the routine along in a register.
    TEST(MacosProcessSyscalls, BsdthreadCreateStartsTheThreadAtTheRegisteredTrampoline)
    {
        const auto emu = macos_test::make_emulator();
        constexpr uint64_t trampoline = 0x1234000;
        constexpr uint64_t routine = 0x5678000;
        constexpr uint64_t argument = 0xABCD;
        constexpr uint64_t stack = 0x300010000ULL;
        constexpr uint64_t pthread = 0x300020000ULL;

        // The kernel writes the new thread's port into the struct's TSD before it ever runs, so the
        // struct has to be memory a kernel could write -- an address alone is not enough any more.
        ASSERT_TRUE(emu->memory.allocate_memory(pthread, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        // Registered twice with different values on purpose: asserting against a single one cannot tell
        // "remembered what the caller passed" from "happens to equal the constant in the test".
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0xDEAD000});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0});
        run_syscall(*emu, macos_test::movz_x(16, 366, 0)); // bsdthread_register
        ASSERT_EQ(emu->process.pthread_thread_start, 0xDEAD000ull);

        emu->emu().reg(sogen::arm64_register::x0, trampoline);
        run_syscall_at(*emu, code_base + 0x20, macos_test::movz_x(16, 366, 0));
        ASSERT_EQ(emu->process.pthread_thread_start, trampoline);

        const auto before = emu->process.threads.size();

        emu->emu().reg(sogen::arm64_register::x0, routine);
        emu->emu().reg(sogen::arm64_register::x1, argument);
        emu->emu().reg(sogen::arm64_register::x2, stack);
        emu->emu().reg(sogen::arm64_register::x3, pthread);
        emu->emu().reg(sogen::arm64_register::x4, uint64_t{sogen::MACOS_PTHREAD_START_CUSTOM});
        run_syscall_at(*emu, code_base + 0x40, macos_test::movz_x(16, 360, 0)); // bsdthread_create

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), pthread) << "the call answers with the pthread it was given";
        ASSERT_EQ(emu->process.threads.size(), before + 1);

        const auto& created = std::prev(emu->process.threads.end())->second;
        EXPECT_EQ(created.saved_regs.pc, trampoline);
        EXPECT_EQ(created.saved_regs.sp, stack);
        EXPECT_EQ(created.saved_regs.x[0], pthread);
        EXPECT_EQ(created.saved_regs.x[2], routine);
        EXPECT_EQ(created.saved_regs.x[3], argument);
        EXPECT_NE(created.saved_regs.x[5] & sogen::MACOS_PTHREAD_START_TSD_BASE_SET, 0u)
            << "the thread pointer is already live, so _pthread_start must not go back for it";
        EXPECT_EQ(created.saved_regs.tpidrro_el0, pthread + sogen::MACOS_PTHREAD_STRUCT_TO_TSD_OFFSET);
    }

    TEST(MacosProcessSyscalls, BsdthreadCreateRefusesWhatItCannotHonour)
    {
        const auto emu = macos_test::make_emulator();

        // No trampoline registered yet, so there is nowhere for a thread to start.
        emu->emu().reg(sogen::arm64_register::x4, uint64_t{sogen::MACOS_PTHREAD_START_CUSTOM});
        run_syscall(*emu, macos_test::movz_x(16, 360, 0));
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL));
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0x1234000});
        run_syscall_at(*emu, code_base + 0x40, macos_test::movz_x(16, 366, 0));

        // The kernel-allocated form would need the emulator to know libpthread's struct layout.
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{0x300010000ULL});
        emu->emu().reg(sogen::arm64_register::x3, uint64_t{0x300020000ULL});
        emu->emu().reg(sogen::arm64_register::x4, uint64_t{0});
        run_syscall_at(*emu, code_base + 0x80, macos_test::movz_x(16, 360, 0));
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOTSUP));
    }

    // One CPU, so a thread keeps it until it gives it up; what matters is that the hand-off lands on
    // something runnable and never on a thread that has already terminated.
    TEST(MacosProcessSyscalls, ResumingPassesTheCpuToARunnableThreadAndSkipsTerminatedOnes)
    {
        const auto emu = macos_test::make_emulator();

        const auto first = emu->process.create_thread(0x300000000ULL, 0, 0x1000);
        const auto second = emu->process.create_thread(0x300010000ULL, 0, 0x2000);
        const auto third = emu->process.create_thread(0x300030000ULL, 0, 0x3000);
        ASSERT_TRUE(emu->activate_thread(first));
        emu->process.threads.at(second).terminated = true;

        ASSERT_TRUE(emu->resume_some_thread());
        EXPECT_EQ(emu->process.active_thread->thread_id, third);

        ASSERT_TRUE(emu->resume_some_thread());
        EXPECT_EQ(emu->process.active_thread->thread_id, first) << "and back round again";

        emu->process.threads.at(third).terminated = true;
        EXPECT_FALSE(emu->resume_some_thread()) << "nothing else is runnable, so the caller keeps the cpu";
    }

    TEST(MacosProcessSyscalls, IdentitySyscallsReportTheProcessContext)
    {
        const auto emu = macos_test::make_emulator();
        emu->process.pid = 4242;

        run_syscall(*emu, 0xD2800290); // mov x16, #20 (getpid)
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 4242u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MacosProcessSyscalls, EachIdentitySyscallReportsItsOwnField)
    {
        const auto emu = macos_test::make_emulator();
        emu->process.pid = 4242;
        emu->process.ppid = 4141;
        emu->process.uid = 505;
        emu->process.euid = 606;
        emu->process.gid = 707;
        emu->process.egid = 808;

        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        const std::array<std::pair<uint32_t, uint64_t>, 6> cases{{
            {0xD2800290, 4242}, // mov x16, #20  (getpid)
            {0xD28004F0, 4141}, // mov x16, #39  (getppid)
            {0xD2800310, 505},  // mov x16, #24  (getuid)
            {0xD2800330, 606},  // mov x16, #25  (geteuid)
            {0xD28005F0, 707},  // mov x16, #47  (getgid)
            {0xD2800570, 808},  // mov x16, #43  (getegid)
        }};

        uint64_t address = code_base;
        for (const auto& [instruction, expected] : cases)
        {
            emu->emu().reg(sogen::arm64_register::x0, ~expected);
            run_syscall_at(*emu, address, instruction);
            EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), expected) << "instruction 0x" << std::hex << instruction;
            EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
            address += 0x40;
        }
    }

    TEST(MacosProcessSyscalls, GetuidReportsFiveHundredAndOne)
    {
        const auto emu = macos_test::make_emulator();

        run_syscall(*emu, 0xD2800310); // mov x16, #24 (getuid)
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 501u) << "Darwin's first regular user is 501, not Linux's 1000";
    }

    TEST(MacosProcessSyscalls, IssetugidReportsZero)
    {
        const auto emu = macos_test::make_emulator();
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0xDEAD});

        run_syscall(*emu, 0xD28028F0); // mov x16, #327 (issetugid)
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MacosProcessSyscalls, ExitRecordsTheStatusAndStopsTheEmulator)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800120, // mov x0, #9
                                         0xD2800030, // mov x16, #1 (exit)
                                         0xD4001001, // svc #0x80
                                         0xD4200000, // brk #0
                                     });

        emu->start(8);

        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, 9);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit);
    }

    TEST(MacosProcessSyscalls, ExitTerminatesTheActiveThread)
    {
        const auto emu = macos_test::make_emulator();
        emu->process.setup(emu->emu(), emu->memory, code_base, {"/bin/hello"}, {}, "/bin/hello");
        ASSERT_NE(emu->process.active_thread, nullptr);

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800120, // mov x0, #9
                                         0xD2800030, // mov x16, #1 (exit)
                                         0xD4001001, // svc #0x80
                                     });

        emu->start(3);

        ASSERT_NE(emu->process.active_thread, nullptr);
        EXPECT_TRUE(emu->process.active_thread->terminated);
        EXPECT_EQ(emu->process.active_thread->exit_code, 9);
    }

    // SQLite writes this into CoreData's .store-conch before it will take a lock, so the run stops here
    // if it is missing. It must be a synthetic id -- the host's real hardware UUID follows the analyst
    // around -- and it must be the same one every run, or a conch written by one run confuses the next.
    TEST(MacosProcessSyscalls, GethostuuidReportsAStableSyntheticIdentity)
    {
        constexpr std::array<uint8_t, 16> expected{0x53, 0x6F, 0x67, 0x65, 0x6E, 0x45, 0x4D, 0x55,
                                                   0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(buffer_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.set_memory(buffer_base, untouched, 32);

        macos_test::write_guest_code(*emu, code_base, {macos_test::movz_x(16, 142, 0), 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, buffer_base);
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        std::array<uint8_t, 32> bytes{};
        emu->memory.read_memory(buffer_base, bytes.data(), bytes.size());
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), bytes.begin()));
        EXPECT_EQ(std::count(bytes.begin() + 16, bytes.end(), untouched), 16) << "gethostuuid writes exactly sixteen bytes";

        // RFC 4122 keeps the version in the high nibble of byte 6 and the variant in the top bits of
        // byte 8; a guest that parses the id rather than copying it rejects anything else.
        EXPECT_EQ(bytes[6] >> 4, 4) << "version 4";
        EXPECT_EQ(bytes[8] & 0xC0, 0x80) << "RFC 4122 variant";
    }

    TEST(MacosProcessSyscalls, GethostuuidFaultsOnAnUnmappedBuffer)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {macos_test::movz_x(16, 142, 0), 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0x500000000ULL});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 14u) << "EFAULT";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosProcessSyscalls, GetentropyFillsTheGuestBuffer)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(buffer_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.set_memory(buffer_base, untouched, 64);

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2803E90, // mov x16, #500 (getentropy)
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x0, buffer_base);
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{32});

        emu->start(2);

        std::array<uint8_t, 64> bytes{};
        emu->memory.read_memory(buffer_base, bytes.data(), bytes.size());
        EXPECT_GT(distinct_bytes(bytes.data(), 32), 8u) << "the requested range must hold entropy, not a constant";
        EXPECT_EQ(std::count(bytes.begin() + 32, bytes.end(), untouched), 32) << "getentropy must not write past the requested length";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MacosProcessSyscalls, GetentropyRejectsOversizedRequests)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(buffer_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.set_memory(buffer_base, untouched, 257);

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2803E90, // mov x16, #500 (getentropy)
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x0, buffer_base);
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{257});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 22u) << "EINVAL";

        std::array<uint8_t, 257> bytes{};
        emu->memory.read_memory(buffer_base, bytes.data(), bytes.size());
        EXPECT_EQ(std::count(bytes.begin(), bytes.end(), untouched), static_cast<long>(bytes.size()))
            << "a rejected request must not have touched the buffer";
    }

    TEST(MacosProcessSyscalls, GetentropyAcceptsTheDarwinMaximum)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(buffer_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.set_memory(buffer_base, 0, 256);

        macos_test::write_guest_code(*emu, code_base, {0xD2803E90, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, buffer_base);
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{256});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u) << "256 is the limit, not one past it";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
    }

    TEST(MacosProcessSyscalls, GetentropyFaultsInsteadOfThrowingOnAnUnmappedBuffer)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0xD2803E90, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0x500000000ULL});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{32});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 14u) << "EFAULT";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::backend_error);
    }

    TEST(MacosProcessSyscalls, GetentropyReadsItsArgumentsThroughTheIndirectSyscall)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(buffer_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.set_memory(buffer_base, untouched, 32);

        macos_test::write_guest_code(*emu, code_base, {0xD4001001}); // svc #0x80
        emu->emu().reg(sogen::arm64_register::x16, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{500});
        emu->emu().reg(sogen::arm64_register::x1, buffer_base);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{32});

        emu->start(1);

        std::array<uint8_t, 32> bytes{};
        emu->memory.read_memory(buffer_base, bytes.data(), bytes.size());
        EXPECT_GT(distinct_bytes(bytes.data(), bytes.size()), 8u)
            << "a handler reading x0/x1 directly would take 500 as the buffer address";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MacosProcessSyscalls, ThreadSelfidReportsTheActiveThread)
    {
        const auto emu = macos_test::make_emulator();
        emu->process.create_thread(0x200000000ULL, 0x1000, code_base);
        emu->process.create_thread(0x200100000ULL, 0x1000, code_base);
        emu->process.setup(emu->emu(), emu->memory, code_base, {"/bin/hello"}, {}, "/bin/hello");
        ASSERT_NE(emu->process.active_thread, nullptr);
        const auto expected = emu->process.active_thread->thread_id;
        ASSERT_EQ(expected, 3u) << "the active thread must not be the first one, or the assertion below is vacuous";

        run_syscall(*emu, 0xD2802E90); // mov x16, #372 (thread_selfid)
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), expected);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MacosProcessSyscalls, ThreadSelfidReportsZeroWithoutAnActiveThread)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_EQ(emu->process.active_thread, nullptr);

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0xDEAD});
        run_syscall(*emu, 0xD2802E90); // mov x16, #372 (thread_selfid)

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::backend_error);
    }

    TEST(MacosProcessSyscalls, CsopsRefusesEveryOperation)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        run_syscall_at(*emu, code_base, 0xD2801530); // mov x16, #169 (csops)
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 22u) << "EINVAL";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);

        run_syscall_at(*emu, code_base + 0x40, 0xD2801550); // mov x16, #170 (csops_audittoken)
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 22u) << "EINVAL";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    // Only the identity flavours are answered. What has to stay true is that everything else is refused
    // rather than answered with zeroes, and that the refusal is reported under the syscall's own name.
    TEST(MacosProcessSyscalls, ProcInfoRefusesWhatItDoesNotModel)
    {
        const auto emu = macos_test::make_emulator();

        std::string observed_name{};
        emu->callbacks.on_syscall = [&](uint64_t, const std::string_view name) {
            observed_name = name;
            return sogen::instruction_hook_continuation::run_instruction;
        };

        // An unknown call number, which is what x0 = 0 is.
        run_syscall(*emu, 0xD2802A10); // mov x16, #336 (proc_info)

        EXPECT_EQ(observed_name, "proc_info") << "an unregistered handler would log <unknown>";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 3u) << "ESRCH: no such process";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);

        // A pid other than this one is a question about a machine that is not being modelled.
        emu->emu().reg(sogen::arm64_register::x0, 2);
        emu->emu().reg(sogen::arm64_register::x1, emu->process.pid + 1);
        emu->emu().reg(sogen::arm64_register::x2, 3);
        run_syscall_at(*emu, code_base + 0x100, 0xD2802A10);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 3u) << "ESRCH for another pid";

        // A flavour that is modelled for no process at all.
        emu->emu().reg(sogen::arm64_register::x0, 2);
        emu->emu().reg(sogen::arm64_register::x1, emu->process.pid);
        emu->emu().reg(sogen::arm64_register::x2, 7); // PROC_PIDREGIONINFO
        run_syscall_at(*emu, code_base + 0x200, 0xD2802A10);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 78u) << "ENOSYS for an unmodelled flavour";
    }

    TEST(MacosProcessSyscalls, BsdthreadRegisterAndMacSyscallSucceed)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0xDEAD});
        run_syscall_at(*emu, code_base, 0xD2802DD0); // mov x16, #366 (bsdthread_register)
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), sogen::MACOS_PTHREAD_SUPPORTED_FEATURES)
            << "the return value is libpthread's feature mask, not a status";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0xDEAD});
        run_syscall_at(*emu, code_base + 0x40, 0xD2802FB0); // mov x16, #381 (mac_syscall)
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

}

namespace
{
    struct rlimit_pair
    {
        uint64_t current;
        uint64_t maximum;
    };

    rlimit_pair read_rlimit(sogen::macos_emulator& emu, const uint64_t address)
    {
        rlimit_pair value{};
        emu.memory.read_memory(address, &value, sizeof(value));
        return value;
    }

    // libsystem_c asks for the stack and descriptor limits during start-up, before main. Without them a
    // real .app stops 4.7M instructions in, having done everything else correctly.
    TEST(MacosProcessSyscalls, GetrlimitReportsUsableLimits)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t out = 0x300000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(out, 0x1000, sogen::memory_permission::read_write));

        constexpr uint64_t limit_stack = 3;
        constexpr uint64_t limit_nofile = 8;

        emu->emu().reg(sogen::arm64_register::x0, limit_stack);
        emu->emu().reg(sogen::arm64_register::x1, out);
        run_syscall(*emu, macos_test::movz_x(16, 194, 0));
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);

        const auto stack = read_rlimit(*emu, out);
        EXPECT_GT(stack.current, 0u) << "a zero stack limit tells the guest it has no stack";
        EXPECT_GE(stack.maximum, stack.current);

        emu->emu().reg(sogen::arm64_register::x0, limit_nofile);
        emu->emu().reg(sogen::arm64_register::x1, out + 0x40);
        run_syscall_at(*emu, code_base + 0x100, macos_test::movz_x(16, 194, 0));

        const auto files = read_rlimit(*emu, out + 0x40);
        EXPECT_GT(files.current, 2u) << "the guest already holds stdin, stdout and stderr";
        EXPECT_GE(files.maximum, files.current);
    }

    TEST(MacosProcessSyscalls, GetrlimitRejectsAnUnknownResourceAndABadPointer)
    {
        const auto emu = macos_test::make_emulator();

        emu->emu().reg(sogen::arm64_register::x0, 999);
        emu->emu().reg(sogen::arm64_register::x1, 0x300000000ULL);
        run_syscall(*emu, macos_test::movz_x(16, 194, 0));
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u) << "an unknown resource must fail";

        emu->emu().reg(sogen::arm64_register::x0, 3);
        emu->emu().reg(sogen::arm64_register::x1, 0);
        run_syscall_at(*emu, code_base + 0x200, macos_test::movz_x(16, 194, 0));
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u) << "a null destination must fail";
    }

    // Accepted and discarded. A process lowering its own limits is telling the kernel about itself, and
    // reporting failure would make a guest that checks the result abort a start-up it could complete.
    TEST(MacosProcessSyscalls, SetrlimitIsAcceptedAndDoesNotChangeWhatIsReported)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t buffer = 0x300000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(buffer, 0x1000, sogen::memory_permission::read_write));

        constexpr uint64_t limit_nofile = 8;

        emu->emu().reg(sogen::arm64_register::x0, limit_nofile);
        emu->emu().reg(sogen::arm64_register::x1, buffer);
        run_syscall(*emu, macos_test::movz_x(16, 194, 0));
        const auto before = read_rlimit(*emu, buffer);

        const rlimit_pair lowered{.current = 16, .maximum = 16};
        emu->memory.write_memory(buffer + 0x40, &lowered, sizeof(lowered));

        emu->emu().reg(sogen::arm64_register::x0, limit_nofile);
        emu->emu().reg(sogen::arm64_register::x1, buffer + 0x40);
        run_syscall_at(*emu, code_base + 0x100, macos_test::movz_x(16, 195, 0));
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u) << "setrlimit must succeed";

        emu->emu().reg(sogen::arm64_register::x0, limit_nofile);
        emu->emu().reg(sogen::arm64_register::x1, buffer + 0x80);
        run_syscall_at(*emu, code_base + 0x200, macos_test::movz_x(16, 194, 0));
        const auto after = read_rlimit(*emu, buffer + 0x80);

        EXPECT_EQ(after.current, before.current);
        EXPECT_EQ(after.maximum, before.maximum);
    }
}

namespace
{
#pragma pack(push, 1)

    struct guest_auditinfo_addr
    {
        uint32_t auid;
        uint32_t mask_success;
        uint32_t mask_failure;
        uint32_t termid_port;
        uint32_t termid_type;
        uint32_t termid_addr[4];
        uint32_t asid;
        uint64_t flags;
    };

#pragma pack(pop)

    static_assert(sizeof(guest_auditinfo_addr) == 48);

    // libsystem_c reads the audit session while working out who the process is, which anything that
    // checks its own privileges reaches early. Without it a real binary stops 10.1M instructions in.
    TEST(MacosProcessSyscalls, GetauditAddrReportsTheSessionTheTaskAlreadyClaims)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t out = 0x300000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(out, 0x1000, sogen::memory_permission::read_write));

        emu->emu().reg(sogen::arm64_register::x0, out);
        emu->emu().reg(sogen::arm64_register::x1, sizeof(guest_auditinfo_addr));
        run_syscall(*emu, macos_test::movz_x(16, 357, 0));

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);

        guest_auditinfo_addr info{};
        emu->memory.read_memory(out, &info, sizeof(info));

        // The same session the task port reports through task_audit_token; two answers that disagree
        // would let a guest catch the emulator contradicting itself.
        EXPECT_EQ(info.asid, emu->process.audit_session_id);
        EXPECT_EQ(info.auid, emu->process.uid);
    }

    TEST(MacosProcessSyscalls, GetauditAddrRejectsAShortBufferAndANullPointer)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t out = 0x300000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(out, 0x1000, sogen::memory_permission::read_write));

        emu->emu().reg(sogen::arm64_register::x0, out);
        emu->emu().reg(sogen::arm64_register::x1, sizeof(guest_auditinfo_addr) - 1);
        run_syscall(*emu, macos_test::movz_x(16, 357, 0));
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u) << "a short buffer must fail";

        emu->emu().reg(sogen::arm64_register::x0, 0);
        emu->emu().reg(sogen::arm64_register::x1, sizeof(guest_auditinfo_addr));
        run_syscall_at(*emu, code_base + 0x100, macos_test::movz_x(16, 357, 0));
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u) << "a null destination must fail";
    }
}

namespace
{
    // gettid is syscall 286 and getaudit_addr is 357. Registering the second on the first's number wrote a
    // 48-byte struct where CoreFoundation had reserved two adjacent 4-byte stack slots, which took out the
    // caller's frame and its stack canary -- the abort that followed looked like CoreFoundation choosing
    // to fail, and stayed that way until the backtrace could name ___stack_chk_fail.
    //
    // So the test is on the bytes, not the values: a syscall that writes past what its caller reserved is
    // not visible in what it returns.
    TEST(MacosProcessSyscalls, GettidWritesTwoWordsAndNotAByteMore)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t scratch = 0x300000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, 0x1000, sogen::memory_permission::read_write));

        std::vector<uint8_t> poison(0x100, 0xCD);
        emu->memory.write_memory(scratch, poison.data(), poison.size());

        emu->emu().reg(sogen::arm64_register::x0, scratch);
        emu->emu().reg(sogen::arm64_register::x1, scratch + 4);
        run_syscall(*emu, macos_test::movz_x(16, 286, 0));

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        uint32_t uid{};
        uint32_t gid{};
        emu->memory.read_memory(scratch, &uid, sizeof(uid));
        emu->memory.read_memory(scratch + 4, &gid, sizeof(gid));
        EXPECT_EQ(uid, emu->process.uid);
        EXPECT_EQ(gid, emu->process.gid);

        std::vector<uint8_t> after(0x100);
        emu->memory.read_memory(scratch, after.data(), after.size());

        for (size_t i = 8; i < after.size(); ++i)
        {
            ASSERT_EQ(after[i], 0xCDu) << "byte " << i << " past the two words the caller reserved was overwritten";
        }
    }

    TEST(MacosProcessSyscalls, GetauditAddrAnswersOnItsOwnNumber)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t out = 0x300000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(out, 0x1000, sogen::memory_permission::read_write));

        emu->emu().reg(sogen::arm64_register::x0, out);
        emu->emu().reg(sogen::arm64_register::x1, 48);
        run_syscall(*emu, macos_test::movz_x(16, 357, 0));

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u) << "357 is getaudit_addr";

        // auid, mask (2), termid port and type, termid addr (4) — 36 bytes before the session id.
        uint32_t asid{};
        emu->memory.read_memory(out + 36, &asid, sizeof(asid));
        EXPECT_EQ(asid, emu->process.audit_session_id);
    }

    // libpthread stores what bsdthread_register returns as __pthread_supported_features and treats zero
    // as "libpthread has not been initialized", which it reports through os_crash with exactly that
    // string before trapping. Reporting plain success killed every guest that reached a workqueue path.
    TEST(ProcessSyscalls, BsdthreadRegisterReportsThePthreadFeatureMask)
    {
        const auto emu = macos_test::make_emulator();

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0x1000});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0x2000});
        run_syscall(*emu, macos_test::movz_x(16, 366, 0));

        const auto features = emu->emu().reg(sogen::arm64_register::x0);
        EXPECT_NE(features, 0u) << "zero is what libpthread reads as uninitialised";
        EXPECT_EQ(features, sogen::MACOS_PTHREAD_SUPPORTED_FEATURES);

        // Measured on build 25G76 by reading __pthread_supported_features out of a live native process.
        // libdispatch on this build aborts in _libdispatch_init if the kevent and workloop bits are
        // clear, so the mask cannot be narrowed to only what sogen serves.
        EXPECT_EQ(features, 0x400011DFu);
    }

    // arm64e libpthread registers both entry points signed. The kernel ptrauth_strip()s them, because it
    // enters a new thread by writing pc and not through an authenticating branch; keeping the signature
    // starts the first real pthread_create() at a pc with the signature still in its top bits, which
    // faults before the thread's first instruction.
    TEST(ProcessSyscalls, BsdthreadRegisterStripsThePointerAuthenticationCode)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t signature = 0x1600ULL << 48;

        emu->emu().reg(sogen::arm64_register::x0, signature | 0x1804fac14ULL);
        emu->emu().reg(sogen::arm64_register::x1, signature | 0x1804fac08ULL);
        run_syscall(*emu, macos_test::movz_x(16, 366, 0));

        EXPECT_EQ(emu->process.pthread_thread_start, 0x1804fac14ull);
        EXPECT_EQ(emu->process.pthread_wqthread, 0x1804fac08ull);
    }

    TEST(ProcessSyscalls, WorkqueueSetupSucceedsAndScheduledWorkIsRefused)
    {
        const auto emu = macos_test::make_emulator();

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{sogen::MACOS_WQOPS_SETUP_DISPATCH});
        run_syscall(*emu, macos_test::movz_x(16, 368, 0));
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u) << "handing over the dispatch entry points asks for no work";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);

        // Anything else asks the kernel to run work on a thread it owns. Answering success would leave
        // the caller waiting on a completion that never comes, and a hang reads far worse than an errno.
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1}); // WQOPS_QUEUE_ADD
        run_syscall_at(*emu, code_base + 0x20, macos_test::movz_x(16, 368, 0));
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOTSUP));
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u) << "Darwin reports errors through the carry";
    }

    // The 2026-08-27 Stage A spec ("Measured 2026-08-27"): kevent_qos/kevent_id apply a changelist and
    // answer with the count of events actually placed. Zero is still the answer here, but a truthful
    // one -- nothing is pending -- where the old stub accepted registrations and forgot them.
    TEST(ProcessSyscalls, KeventAppliesChangesAndAnswersWithThePlacedCount)
    {
        const auto emu = macos_test::make_emulator();

        run_syscall(*emu, macos_test::movz_x(16, 362, 0)); // kqueue
        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);
        const auto kq = emu->emu().reg(sogen::arm64_register::x0);

        emu->emu().reg(sogen::arm64_register::x0, kq);
        run_syscall_at(*emu, code_base + 0x20, macos_test::movz_x(16, 374, 0)); // kevent_qos
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u) << "a fresh kqueue has nothing pending to place";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);

        // kevent_id with the measured workloop call flags: arg 0 is the workloop's dynamic kq id, not
        // a descriptor, and an empty changelist places nothing.
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0x300518520});
        emu->emu().reg(sogen::arm64_register::x7, uint64_t{0x403});             // IMMEDIATE | ERROR_EVENTS | WORKLOOP
        run_syscall_at(*emu, code_base + 0x40, macos_test::movz_x(16, 375, 0)); // kevent_id
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);
    }

    // Measured on 25G76: __disable_threadsignal answers 0 for every argument, and afterwards the thread
    // is not a signal target at all -- pthread_kill answers ESRCH for a real signal and for the signal-0
    // existence probe alike, and the handler that would have run does not.
    TEST(ProcessSyscalls, DisableThreadsignalTakesTheThreadOutOfTheSignalNamespace)
    {
        const auto emu = macos_test::make_emulator();
        emu->process.setup(emu->emu(), emu->memory, code_base, {"/bin/hello"}, {}, "/bin/hello");
        ASSERT_NE(emu->process.active_thread, nullptr);

        // setup owns code_base, so the syscalls are assembled somewhere it does not reach.
        constexpr uint64_t scratch = 0x400000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        const auto self = emu->process.active_thread->thread_id;
        const auto port = emu->mach.thread_self_for(self);

        // Signal 0 to a thread that still takes signals is the control: it has to succeed, or the ESRCH
        // below would prove nothing.
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{port});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0});
        run_syscall_at(*emu, scratch, macos_test::movz_x(16, 328, 0)); // __pthread_kill
        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);
        ASSERT_FALSE(emu->process.threads.at(self).signals_disabled);

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{1});
        run_syscall_at(*emu, scratch + 0x20, macos_test::movz_x(16, 331, 0)); // __disable_threadsignal
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);
        EXPECT_TRUE(emu->process.threads.at(self).signals_disabled);

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{port});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0});
        run_syscall_at(*emu, scratch + 0x40, macos_test::movz_x(16, 328, 0));
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ESRCH))
            << "the existence probe fails too, so a caller cannot tell the thread is still running";
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);

        // A real signal must not terminate the guest: it never reaches delivery.
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{port});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{30}); // SIGUSR1
        run_syscall_at(*emu, scratch + 0x60, macos_test::movz_x(16, 328, 0));
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ESRCH));
        EXPECT_FALSE(emu->process.threads.at(self).terminated) << "a refused signal kills nothing";
    }

    // Measured on 25G76 from an ordinary process: every persona operation refuses, but not with the
    // same errno, and PERSONA_OP_GET's ESRCH is the one a caller reads as "this process has no
    // persona" rather than "you were denied".
    TEST(ProcessSyscalls, PersonaRefusesEveryOperationTheWayTheKernelDoes)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        const auto call_persona = [&](const uint64_t offset, const uint32_t operation, const uint64_t idlen) {
            emu->emu().reg(sogen::arm64_register::x0, uint64_t{operation});
            emu->emu().reg(sogen::arm64_register::x1, uint64_t{0});
            emu->emu().reg(sogen::arm64_register::x2, uint64_t{0});
            emu->emu().reg(sogen::arm64_register::x3, uint64_t{0});
            emu->emu().reg(sogen::arm64_register::x4, idlen);
            run_syscall_at(*emu, code_base + offset, macos_test::movz_x(16, 494, 0));

            EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u)
                << "operation " << operation << " refuses, so the carry is raised";
            return emu->emu().reg(sogen::arm64_register::x0);
        };

        EXPECT_EQ(call_persona(0x00, sogen::MACOS_PERSONA_OP_GET, 0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ESRCH))
            << "no persona is not a denial";

        EXPECT_EQ(call_persona(0x20, sogen::MACOS_PERSONA_OP_ALLOC, 0), static_cast<uint64_t>(sogen::macos_errno::MACOS_EPERM));
        EXPECT_EQ(call_persona(0x40, sogen::MACOS_PERSONA_OP_INFO, 0), static_cast<uint64_t>(sogen::macos_errno::MACOS_EPERM));
        EXPECT_EQ(call_persona(0x60, sogen::MACOS_PERSONA_OP_PIDINFO, 0), static_cast<uint64_t>(sogen::macos_errno::MACOS_EPERM));
        EXPECT_EQ(call_persona(0x80, sogen::MACOS_PERSONA_OP_SUPPORT, 0), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL));
        EXPECT_EQ(call_persona(0xA0, 0, 0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOSYS));

        // The two search operations store the match count before they refuse.
        constexpr uint64_t idlen_address = buffer_base;
        ASSERT_TRUE(emu->memory.allocate_memory(idlen_address, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const size_t poison = 0x4141414141414141ULL;
        ASSERT_TRUE(emu->memory.try_write_memory(idlen_address, &poison, sizeof(poison)));

        EXPECT_EQ(call_persona(0xC0, sogen::MACOS_PERSONA_OP_FIND, idlen_address), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL));

        size_t written = poison;
        ASSERT_TRUE(emu->memory.try_read_memory(idlen_address, &written, sizeof(written)));
        EXPECT_EQ(written, 0u) << "PERSONA_OP_FIND stores the match count before refusing";
    }
}

#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <array>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t data_base = 0x300000000ULL;

    TEST(MacosHandlerCallbacks, AnonymousMmapReportsACommit)
    {
        const auto emu = macos_test::make_emulator();

        uint64_t reported_address = 0;
        uint64_t reported_length = 0;
        auto reported_permissions = sogen::memory_permission::none;
        bool reported_commit = false;

        emu->callbacks.on_memory_allocate = [&](const uint64_t address, const uint64_t length, const sogen::memory_permission permissions,
                                                const bool commit) {
            reported_address = address;
            reported_length = length;
            reported_permissions = permissions;
            reported_commit = commit;
        };

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD28018B0, // mov x16, #197 (mmap)
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x0, 0);
        emu->emu().reg(sogen::arm64_register::x1, sogen::MACOS_PAGE_SIZE);
        emu->emu().reg(sogen::arm64_register::x2, 3);      // PROT_READ|PROT_WRITE
        emu->emu().reg(sogen::arm64_register::x3, 0x1002); // MAP_PRIVATE|MAP_ANON
        emu->emu().reg(sogen::arm64_register::x4, static_cast<uint64_t>(-1));
        emu->emu().reg(sogen::arm64_register::x5, 0);

        emu->start(2);

        EXPECT_NE(reported_address, 0u);
        EXPECT_EQ(reported_length, sogen::MACOS_PAGE_SIZE);
        EXPECT_EQ(reported_permissions, sogen::memory_permission::read_write);
        EXPECT_TRUE(reported_commit);
    }

    TEST(MacosHandlerCallbacks, MprotectReportsTheNewPermissions)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        auto reported = sogen::memory_permission::none;
        emu->callbacks.on_memory_protect = [&](uint64_t, uint64_t, const sogen::memory_permission permissions) { reported = permissions; };

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800950, // mov x16, #74 (mprotect)
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, sogen::MACOS_PAGE_SIZE);
        emu->emu().reg(sogen::arm64_register::x2, 5); // PROT_READ|PROT_EXEC

        emu->start(2);

        EXPECT_EQ(reported, sogen::memory_permission::read_exec);
    }

    TEST(MacosHandlerCallbacks, MunmapReportsTheRelease)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        uint64_t reported_address = 0;
        uint64_t reported_length = 0;
        emu->callbacks.on_memory_release = [&](const uint64_t address, const uint64_t length) {
            reported_address = address;
            reported_length = length;
        };

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800930, // mov x16, #73 (munmap)
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, sogen::MACOS_PAGE_SIZE);

        emu->start(2);

        EXPECT_EQ(reported_address, data_base);
        EXPECT_EQ(reported_length, sogen::MACOS_PAGE_SIZE);
    }

    TEST(MacosHandlerCallbacks, ThreadCreationReportsWhereTheThreadStarts)
    {
        const auto emu = macos_test::make_emulator();

        uint64_t reported_id = 0;
        uint64_t reported_start = 0;
        uint64_t reported_argument = 0;
        emu->callbacks.on_thread_create = [&](const uint64_t id, const uint64_t start, const uint64_t argument) {
            reported_id = id;
            reported_start = start;
            reported_argument = argument;
        };

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0x1234000});
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2801250, // mov x16, #146 overwritten below
                                         0xD4001001,
                                     });
        macos_test::write_guest_code(*emu, code_base, {macos_test::movz_x(16, 366, 0), 0xD4001001});
        emu->start(2);

        // The routine the caller named, not the trampoline the thread actually begins at: the trampoline
        // is the same address for every thread and says nothing about which one this is.
        // The kernel writes the new thread's port into the pthread struct's TSD, so it has to be memory.
        ASSERT_TRUE(emu->memory.allocate_memory(0x300020000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0x5678000});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0xABCD});
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{0x300010000ULL});
        emu->emu().reg(sogen::arm64_register::x3, uint64_t{0x300020000ULL});
        emu->emu().reg(sogen::arm64_register::x4, uint64_t{sogen::MACOS_PTHREAD_START_CUSTOM});
        const std::array<uint32_t, 2> words{macos_test::movz_x(16, 360, 0), 0xD4001001};
        emu->memory.write_memory(code_base + 0x40, words.data(), sizeof(words));
        emu->emu().reg(sogen::arm64_register::pc, code_base + 0x40);
        emu->start(2);

        EXPECT_NE(reported_id, 0u);
        EXPECT_EQ(reported_start, 0x5678000u);
        EXPECT_EQ(reported_argument, 0xABCDu);
    }

    TEST(MacosHandlerCallbacks, ExitReportsItsStatus)
    {
        const auto emu = macos_test::make_emulator();

        std::optional<int> reported{};
        emu->callbacks.on_process_exit = [&](const int status) { reported = status; };

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800120, // mov x0, #9
                                         0xD2800030, // mov x16, #1 (exit)
                                         0xD4001001, // svc #0x80
                                     });

        emu->start(3);

        ASSERT_TRUE(reported.has_value());
        EXPECT_EQ(*reported, 9);
    }
}

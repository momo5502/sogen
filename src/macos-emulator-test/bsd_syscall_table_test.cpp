#include <gtest/gtest.h>

#include <trace/bsd_syscall_table.hpp>
#include <array>
#include <string_view>
#include <trace/mach_trap_table.hpp>

#ifdef __APPLE__
#include <sys/syscall.h>
#endif

namespace
{
    TEST(BsdSyscallTable, RecordsItsProvenance)
    {
        EXPECT_TRUE(sogen::BSD_SYSCALL_TABLE_XNU_VERSION.starts_with("xnu-"));
        EXPECT_EQ(sogen::BSD_SYSCALL_TABLE_SOURCE_SHA256.size(), 64u);
        EXPECT_GE(sogen::bsd_syscall_table_size(), 500u);
    }

    TEST(BsdSyscallTable, DecodesWriteWithNamedArguments)
    {
        const auto* proto = sogen::find_bsd_syscall_prototype(4);
        ASSERT_NE(proto, nullptr);
        EXPECT_EQ(proto->name, "write");
        ASSERT_EQ(proto->arguments.size(), 3u);
        EXPECT_EQ(proto->arguments[0].name, "fd");
        EXPECT_EQ(proto->arguments[0].type, "int");
        EXPECT_EQ(proto->arguments[1].name, "cbuf");
        EXPECT_EQ(proto->arguments[2].name, "nbyte");
    }

    TEST(BsdSyscallTable, DecodesOpenAndExit)
    {
        const auto* open_proto = sogen::find_bsd_syscall_prototype(5);
        ASSERT_NE(open_proto, nullptr);
        EXPECT_EQ(open_proto->name, "open");
        ASSERT_EQ(open_proto->arguments.size(), 3u);
        EXPECT_EQ(open_proto->arguments[0].name, "path");
        EXPECT_EQ(open_proto->arguments[1].name, "flags");

        const auto* exit_proto = sogen::find_bsd_syscall_prototype(1);
        ASSERT_NE(exit_proto, nullptr);
        EXPECT_EQ(exit_proto->name, "exit");
        ASSERT_EQ(exit_proto->arguments.size(), 1u);
        EXPECT_EQ(exit_proto->arguments[0].name, "rval");
    }

    TEST(BsdSyscallTable, VoidArgumentListIsEmptyNotOneElement)
    {
        const auto* proto = sogen::find_bsd_syscall_prototype(20);
        ASSERT_NE(proto, nullptr);
        EXPECT_EQ(proto->name, "getpid");
        EXPECT_TRUE(proto->arguments.empty());
    }

    TEST(BsdSyscallTable, OutOfRangeNumberYieldsNullptr)
    {
        EXPECT_EQ(sogen::find_bsd_syscall_prototype(0xFFFFFFFFu), nullptr);
        EXPECT_EQ(sogen::find_bsd_syscall_prototype(static_cast<uint32_t>(sogen::bsd_syscall_table_size())), nullptr);
    }

    TEST(BsdSyscallTable, NoPrototypeDeclaresMoreThanEightArguments)
    {
        for (uint32_t i = 0; i < sogen::bsd_syscall_table_size(); ++i)
        {
            const auto* proto = sogen::find_bsd_syscall_prototype(i);
            if (proto != nullptr)
            {
                EXPECT_LE(proto->arguments.size(), 8u) << "syscall " << i << " (" << proto->name << ")";
            }
        }
    }

#ifdef __APPLE__
    TEST(BsdSyscallTable, MatchesTheHostSdkHeader)
    {
        struct sdk_expectation
        {
            uint32_t number{};
            std::string_view name{};
        };

        constexpr std::array<sdk_expectation, 25> expectations{{
            {.number = SYS_syscall, .name = "syscall"},
            {.number = SYS_exit, .name = "exit"},
            {.number = SYS_read, .name = "read"},
            {.number = SYS_write, .name = "write"},
            {.number = SYS_open, .name = "open"},
            {.number = SYS_close, .name = "close"},
            {.number = SYS_getpid, .name = "getpid"},
            {.number = SYS_getuid, .name = "getuid"},
            {.number = SYS_ioctl, .name = "ioctl"},
            {.number = SYS_munmap, .name = "munmap"},
            {.number = SYS_mprotect, .name = "mprotect"},
            {.number = SYS_madvise, .name = "madvise"},
            {.number = SYS_fcntl, .name = "fcntl"},
            {.number = SYS_writev, .name = "writev"},
            {.number = SYS_mmap, .name = "mmap"},
            {.number = SYS_lseek, .name = "lseek"},
            {.number = SYS_sysctl, .name = "sysctl"},
            {.number = SYS_issetugid, .name = "issetugid"},
            {.number = SYS_fstat64, .name = "fstat64"},
            {.number = SYS_openat, .name = "openat"},
            {.number = SYS_dup, .name = "dup"},
            {.number = SYS_flock, .name = "flock"},
            {.number = SYS_ulock_wait, .name = "ulock_wait"},
            {.number = SYS_sysctlbyname, .name = "sysctlbyname"},
            {.number = SYS_fileport_makefd, .name = "fileport_makefd"},
        }};

        for (const auto& expectation : expectations)
        {
            const auto* proto = sogen::find_bsd_syscall_prototype(expectation.number);
            ASSERT_NE(proto, nullptr) << expectation.name;
            EXPECT_EQ(proto->name, expectation.name) << "number " << expectation.number;
        }
    }
#endif

    TEST(MachTrapTable, NamesTheTimeTrapsStageThreeImplements)
    {
        const auto* absolute_time = sogen::find_mach_trap_prototype(3);
        ASSERT_NE(absolute_time, nullptr);
        EXPECT_EQ(absolute_time->name, "mach_absolute_time");

        const auto* continuous_time = sogen::find_mach_trap_prototype(4);
        ASSERT_NE(continuous_time, nullptr);
        EXPECT_EQ(continuous_time->name, "mach_continuous_time");
    }

    TEST(MachTrapTable, KnowsMachMsgAndTaskSelf)
    {
        bool saw_mach_msg = false;
        bool saw_task_self = false;

        for (uint32_t i = 0; i < sogen::mach_trap_table_size(); ++i)
        {
            const auto* proto = sogen::find_mach_trap_prototype(i);
            if (proto == nullptr)
            {
                continue;
            }

            saw_mach_msg = saw_mach_msg || proto->name.starts_with("mach_msg");
            saw_task_self = saw_task_self || proto->name == "task_self_trap";
            EXPECT_LE(proto->argument_count, 9u) << proto->name;
        }

        EXPECT_TRUE(saw_mach_msg);
        EXPECT_TRUE(saw_task_self);
    }

    TEST(MachTrapTable, UnusedIndicesAreNotReported)
    {
        EXPECT_EQ(sogen::find_mach_trap_prototype(0), nullptr);
        EXPECT_EQ(sogen::find_mach_trap_prototype(static_cast<uint32_t>(sogen::mach_trap_table_size())), nullptr);
    }
}

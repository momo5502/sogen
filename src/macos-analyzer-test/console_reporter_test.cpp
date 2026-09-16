#include <gtest/gtest.h>

#include <macos_reporter.hpp>

#include <logger.hpp>

namespace
{
    class capturing_logger
    {
      public:
        capturing_logger()
        {
            this->log_.disable_output(true);
            this->log_.set_sink([this](sogen::color, const std::string_view message) { this->captured_ += message; });
        }

        sogen::logger& get()
        {
            return this->log_;
        }

        const std::string& captured() const
        {
            return this->captured_;
        }

      private:
        sogen::logger log_{};
        std::string captured_{};
    };

    TEST(MacosConsoleReporter, RendersASyscallLineAndItsArgumentRows)
    {
        capturing_logger log{};
        const auto reporter = sogen::create_macos_console_reporter(log.get(), {});

        sogen::macos_syscall_event syscall{};
        syscall.syscall_id = 4;
        syscall.syscall_name = "write";
        syscall.execution.pc = 0x1000003F0;
        syscall.execution.module = "macho_trace_arm64";
        reporter->report(syscall);

        reporter->report(sogen::macos_trace_detail_event{"fd", "1"});
        reporter->report(sogen::macos_trace_detail_event{"cbuf", "\"Hello, sogen!\\n\""});

        EXPECT_NE(log.captured().find("Executing syscall: write (0x4) at 0x1000003f0 (macho_trace_arm64)"), std::string::npos);
        EXPECT_NE(log.captured().find("--> fd: 1"), std::string::npos);
        EXPECT_NE(log.captured().find("--> cbuf: \"Hello, sogen!\\n\""), std::string::npos);
    }

    TEST(MacosConsoleReporter, RendersMachTrapsWithTheirOwnLabel)
    {
        capturing_logger log{};
        const auto reporter = sogen::create_macos_console_reporter(log.get(), {});

        sogen::macos_syscall_event trap{};
        trap.syscall_id = 3;
        trap.syscall_name = "mach_absolute_time";
        trap.is_mach_trap = true;
        reporter->report(trap);

        EXPECT_NE(log.captured().find("Executing mach trap: mach_absolute_time (0x3)"), std::string::npos);
    }

    TEST(MacosConsoleReporter, RendersAnUnlabelledDetailWithoutAColon)
    {
        capturing_logger log{};
        const auto reporter = sogen::create_macos_console_reporter(log.get(), {});

        reporter->report(sogen::macos_trace_detail_event{{}, "<argument decoding failed>"});

        EXPECT_NE(log.captured().find("--> <argument decoding failed>"), std::string::npos);
        EXPECT_EQ(log.captured().find("--> :"), std::string::npos);
    }

    TEST(MacosConsoleReporter, RendersErrnoWithItsName)
    {
        capturing_logger log{};
        const auto reporter = sogen::create_macos_console_reporter(log.get(), {});

        reporter->report(sogen::macos_syscall_error_event{"open", 2, "ENOENT"});
        reporter->report(sogen::macos_syscall_error_event{"weird", 4242, {}});

        EXPECT_NE(log.captured().find("--> Failed: ENOENT (2)"), std::string::npos);
        EXPECT_NE(log.captured().find("--> Failed: errno 4242"), std::string::npos);
    }

    TEST(MacosConsoleReporter, RendersMemoryEvents)
    {
        capturing_logger log{};
        const auto reporter = sogen::create_macos_console_reporter(log.get(), {});

        reporter->report(sogen::macos_memory_allocate_event{0x300000000, 0x4000, "rw-", true});
        reporter->report(sogen::macos_memory_protect_event{0x300000000, 0x4000, "r-x"});
        reporter->report(sogen::macos_memory_release_event{0x300000000, 0x4000});

        EXPECT_NE(log.captured().find("--> Committed 0x300000000 - 0x300004000 (rw-)"), std::string::npos);
        EXPECT_NE(log.captured().find("--> Changing protection at 0x300000000-0x300004000 to r-x"), std::string::npos);
        EXPECT_NE(log.captured().find("--> Releasing 0x300000000 - 0x300004000"), std::string::npos);
    }

    TEST(MacosConsoleReporter, ConciseModeDropsMemoryAndGenericActivity)
    {
        capturing_logger log{};
        const auto reporter = sogen::create_macos_console_reporter(log.get(), {.silent = false, .concise = true});

        reporter->report(sogen::macos_memory_allocate_event{0x300000000, 0x4000, "rw-", true});
        reporter->report(sogen::macos_generic_activity_event{"noise"});
        reporter->report(sogen::macos_trace_detail_event{"fd", "1"});

        EXPECT_EQ(log.captured().find("Committed"), std::string::npos);
        EXPECT_EQ(log.captured().find("noise"), std::string::npos);
        EXPECT_NE(log.captured().find("--> fd: 1"), std::string::npos);
    }

    TEST(MacosConsoleReporter, SilentModeEmitsOnlyGuestOutput)
    {
        capturing_logger log{};
        const auto reporter = sogen::create_macos_console_reporter(log.get(), {.silent = true});

        sogen::macos_syscall_event syscall{};
        syscall.syscall_name = "write";
        reporter->report(syscall);
        reporter->report(sogen::macos_trace_detail_event{"fd", "1"});

        EXPECT_TRUE(log.captured().empty());
    }

    TEST(MacosConsoleReporter, CallCountPrefixIsOptional)
    {
        capturing_logger log{};
        const auto reporter = sogen::create_macos_console_reporter(log.get(), {.prepend_call_count = true});

        sogen::macos_syscall_event syscall{};
        syscall.call_count = 7;
        syscall.syscall_id = 20;
        syscall.syscall_name = "getpid";
        syscall.execution.thread_id = 3;
        reporter->report(syscall);

        // The thread id is not optional: a multithreaded guest interleaves its syscalls, and a trace
        // that does not say who called what cannot be read once a workqueue pool is running.
        EXPECT_NE(log.captured().find("[7] [t3] Executing syscall: getpid (0x14)"), std::string::npos);
    }
}

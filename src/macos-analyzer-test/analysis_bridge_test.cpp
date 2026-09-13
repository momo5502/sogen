#include <gtest/gtest.h>

#include <macos_analysis.hpp>

#include <backend_selection.hpp>

#include <array>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t data_base = 0x300000000ULL;

    class recording_reporter final : public sogen::macos_analysis_reporter
    {
      public:
        void report(const sogen::macos_analysis_event& event) override
        {
            this->events.push_back(event);
        }

        template <typename T>
        size_t count() const
        {
            size_t total = 0;
            for (const auto& event : this->events)
            {
                total += std::holds_alternative<T>(event) ? 1 : 0;
            }
            return total;
        }

        std::vector<sogen::macos_analysis_event> events{};
    };

    std::unique_ptr<sogen::macos_emulator> make_emulator()
    {
        return std::make_unique<sogen::macos_emulator>(sogen::create_arm64_emulator(), std::filesystem::path{});
    }

    void write_write_syscall(sogen::macos_emulator& emu)
    {
        emu.memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);
        emu.memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        emu.memory.write_memory(data_base, "Hello, sogen!\n", 14);

        const std::array<uint32_t, 4> code{
            0xD2800020, // mov x0, #1
            0xD28001C2, // mov x2, #14
            0xD2800090, // mov x16, #4 (write)
            0xD4001001, // svc #0x80
        };

        emu.memory.write_memory(code_base, code.data(), code.size() * sizeof(uint32_t));
        emu.emu().reg(sogen::arm64_register::pc, code_base);
        emu.emu().reg(sogen::arm64_register::x1, data_base);
    }

    TEST(MacosModuleIndex, ResolvesAnAddressInsideAModule)
    {
        sogen::macos_module_index index{};
        index.add("macho_trace_arm64", 0x100000000ULL, 0x4000);

        EXPECT_EQ(index.find(0x100000000ULL), "macho_trace_arm64");
        EXPECT_EQ(index.find(0x100003FFFULL), "macho_trace_arm64");
        EXPECT_EQ(index.find(0x100004000ULL), "<N/A>");
        EXPECT_EQ(index.find(0), "<N/A>");
    }

    TEST(MacosAnalysisBridge, SyscallProducesAHeaderAndDecodedRows)
    {
        const auto emu = make_emulator();
        recording_reporter reporter{};
        const sogen::macos_analysis_options options{};

        sogen::macos_analysis_context context{.emu = emu.get(), .reporter = &reporter, .options = &options};
        sogen::register_macos_callbacks(context);

        write_write_syscall(*emu);
        emu->start(4);

        ASSERT_EQ(reporter.count<sogen::macos_syscall_event>(), 1u);
        EXPECT_EQ(reporter.count<sogen::macos_trace_detail_event>(), 3u);
        EXPECT_EQ(reporter.count<sogen::macos_stdout_chunk_event>(), 1u);

        const auto& header = std::get<sogen::macos_syscall_event>(reporter.events.front());
        EXPECT_EQ(header.syscall_name, "write");
        EXPECT_EQ(header.syscall_id, 4u);
        EXPECT_FALSE(header.is_mach_trap);
        EXPECT_EQ(header.call_count, 1u);
    }

    TEST(MacosAnalysisBridge, IgnoredSyscallSuppressesItsRowsToo)
    {
        const auto emu = make_emulator();
        recording_reporter reporter{};

        sogen::macos_analysis_options options{};
        options.ignored_syscalls.insert("write");

        sogen::macos_analysis_context context{.emu = emu.get(), .reporter = &reporter, .options = &options};
        sogen::register_macos_callbacks(context);

        write_write_syscall(*emu);
        emu->start(4);

        EXPECT_EQ(reporter.count<sogen::macos_syscall_event>(), 0u);
        EXPECT_EQ(reporter.count<sogen::macos_trace_detail_event>(), 0u);
        EXPECT_EQ(reporter.count<sogen::macos_stdout_chunk_event>(), 1u) << "guest output is never suppressed by --ignore";
    }

    TEST(MacosAnalysisBridge, SkipSyscallsKeepsGuestOutput)
    {
        const auto emu = make_emulator();
        recording_reporter reporter{};

        sogen::macos_analysis_options options{};
        options.skip_syscalls = true;

        sogen::macos_analysis_context context{.emu = emu.get(), .reporter = &reporter, .options = &options};
        sogen::register_macos_callbacks(context);

        write_write_syscall(*emu);
        emu->start(4);

        EXPECT_EQ(reporter.count<sogen::macos_syscall_event>(), 0u);
        EXPECT_EQ(reporter.count<sogen::macos_trace_detail_event>(), 0u);
        EXPECT_EQ(reporter.count<sogen::macos_stdout_chunk_event>(), 1u);
    }

    TEST(MacosPermissionString, RendersTheThreeBits)
    {
        EXPECT_EQ(sogen::macos_permission_string(sogen::memory_permission::none), "---");
        EXPECT_EQ(sogen::macos_permission_string(sogen::memory_permission::read_write), "rw-");
        EXPECT_EQ(sogen::macos_permission_string(sogen::memory_permission::read_exec), "r-x");
        EXPECT_EQ(sogen::macos_permission_string(sogen::memory_permission::all), "rwx");
    }
}

namespace
{
    TEST(MacosAnalysisBridge, MissingExecutableFailsWithoutThrowing)
    {
        sogen::logger log{};
        log.disable_output(true);

        sogen::macos_analysis_options options{};
        options.executable = "/sogen-trace-fixture/definitely-not-here";
        options.max_instructions = 1000;

        EXPECT_NO_THROW({ EXPECT_NE(sogen::run_macos_analysis(options, log), 0); });
    }
}

#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <macos_emulator_callbacks.hpp>
#include <trace/macos_syscall_trace.hpp>
#include <trace/mach_trap_table.hpp>
#include <trace/macos_guest_reader.hpp>

#include <array>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t data_base = 0x300000000ULL;

    TEST(MacosTraceCallbacks, StdoutIsReachedThroughTheCallbackStruct)
    {
        const auto emu = macos_test::make_emulator();

        std::string captured{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { captured.append(data); };

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        constexpr char message[] = "trace\n";
        emu->memory.write_memory(data_base, message, sizeof(message) - 1);

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800020, // mov x0, #1
                                         0xD28000C2, // mov x2, #6
                                         0xD2800090, // mov x16, #4 (write)
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x1, data_base);

        emu->start(4);

        EXPECT_EQ(captured, "trace\n");
    }

    TEST(MacosTraceCallbacks, SyscallCallbackReportsNumberAndName)
    {
        const auto emu = macos_test::make_emulator();

        uint64_t seen_id = 0;
        std::string seen_name{};
        emu->callbacks.on_syscall = [&](const uint64_t id, const std::string_view name) {
            seen_id = id;
            seen_name = name;
            return sogen::instruction_hook_continuation::run_instruction;
        };

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800290, // mov x16, #20 (getpid)
                                         0xD4001001, // svc #0x80
                                     });

        emu->start(2);

        EXPECT_EQ(seen_id, 20u);
        EXPECT_EQ(seen_name, "getpid");
    }

    TEST(MacosTraceCallbacks, TraceSettingsDefaultToDecodingWithA256ByteStringLimit)
    {
        const auto emu = macos_test::make_emulator();

        EXPECT_TRUE(emu->trace.decode_arguments);
        EXPECT_EQ(emu->trace.string_limit, 256u);
        EXPECT_EQ(emu->trace.buffer_preview_limit, 32u);
    }
}

namespace
{
    TEST(MacosTraceCallbacks, FailingSyscallReportsADecodedErrno)
    {
        const auto emu = macos_test::make_emulator();

        std::string seen_name{};
        int64_t seen_error = 0;
        std::string seen_error_name{};
        emu->callbacks.on_syscall_error = [&](const std::string_view name, const int64_t error, const std::string_view error_name) {
            seen_name = name;
            seen_error = error;
            seen_error_name = error_name;
        };

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD28000D0, // mov x16, #6 (close)
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x0, 4242);

        emu->start(2);

        EXPECT_EQ(seen_name, "close");
        EXPECT_EQ(seen_error, 9) << "closing an unopened descriptor must report EBADF";
        EXPECT_EQ(seen_error_name, "EBADF");
    }

    TEST(MacosTraceCallbacks, SucceedingSyscallReportsNoError)
    {
        const auto emu = macos_test::make_emulator();

        bool reported = false;
        emu->callbacks.on_syscall_error = [&](std::string_view, int64_t, std::string_view) { reported = true; };

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800290, // mov x16, #20 (getpid)
                                         0xD4001001, // svc #0x80
                                     });

        emu->start(2);

        EXPECT_FALSE(reported);
    }

    TEST(MacosTraceCallbacks, MachTrapCallbackReportsIndexAndName)
    {
        const auto emu = macos_test::make_emulator();

        uint32_t seen_index = 0;
        std::string seen_name{};
        emu->callbacks.on_mach_trap = [&](const uint32_t index, const std::string_view name) {
            seen_index = index;
            seen_name = name;
            return sogen::instruction_hook_continuation::run_instruction;
        };

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0x92800050, // mov x16, #-3 (mach_absolute_time)
                                         0xD4001001, // svc #0x80
                                     });

        emu->start(2);

        EXPECT_EQ(seen_index, 3u);
        EXPECT_EQ(seen_name, "mach_absolute_time");
    }
}

namespace
{
    std::string detail_value(const std::vector<sogen::macos_trace_detail>& details, const std::string_view label)
    {
        for (const auto& detail : details)
        {
            if (detail.label == label)
            {
                return detail.value;
            }
        }

        return "<absent>";
    }

    TEST(MacosSyscallTrace, WriteDecodesFdBufferAndLength)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.write_memory(data_base, "Hello, sogen!\n", 14);

        const std::array<uint64_t, 8> arguments{1, data_base, 14, 0, 0, 0, 0, 0};
        const auto details = sogen::describe_bsd_syscall(emu->memory, 4, arguments, {});

        ASSERT_EQ(details.size(), 3u);
        EXPECT_EQ(detail_value(details, "fd"), "1");
        EXPECT_EQ(detail_value(details, "cbuf"), "\"Hello, sogen!\\n\"");
        EXPECT_EQ(detail_value(details, "nbyte"), "14");
    }

    TEST(MacosSyscallTrace, OpenDecodesPathAndFlags)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        constexpr char path[] = "/usr/lib/dyld";
        emu->memory.write_memory(data_base, path, sizeof(path));

        const std::array<uint64_t, 8> arguments{data_base, 0x201, 0644, 0, 0, 0, 0, 0};
        const auto details = sogen::describe_bsd_syscall(emu->memory, 5, arguments, {});

        EXPECT_EQ(detail_value(details, "path"), "\"/usr/lib/dyld\"");
        EXPECT_EQ(detail_value(details, "flags"), "O_WRONLY|O_CREAT");
        EXPECT_EQ(detail_value(details, "mode"), "0644");
    }

    TEST(MacosSyscallTrace, MmapDecodesProtectionAndFlags)
    {
        const auto emu = macos_test::make_emulator();

        const std::array<uint64_t, 8> arguments{0, 0x4000, 3, 0x1002, static_cast<uint64_t>(-1), 0, 0, 0};
        const auto details = sogen::describe_bsd_syscall(emu->memory, 197, arguments, {});

        EXPECT_EQ(detail_value(details, "prot"), "PROT_READ|PROT_WRITE");
        EXPECT_EQ(detail_value(details, "flags"), "MAP_PRIVATE|MAP_ANON");
        EXPECT_EQ(detail_value(details, "fd"), "-1");
    }

    TEST(MacosSyscallTrace, BadPointerRendersUnreadableAndDoesNotThrow)
    {
        const auto emu = macos_test::make_emulator();

        const std::array<uint64_t, 8> arguments{0x700000000ULL, 0, 0, 0, 0, 0, 0, 0};

        EXPECT_NO_THROW({
            const auto details = sogen::describe_bsd_syscall(emu->memory, 5, arguments, {});
            EXPECT_EQ(detail_value(details, "path"), "<unreadable>");
        });
    }

    TEST(MacosSyscallTrace, NullPointerRendersNull)
    {
        const auto emu = macos_test::make_emulator();

        const std::array<uint64_t, 8> arguments{0, 0, 0, 0, 0, 0, 0, 0};
        const auto details = sogen::describe_bsd_syscall(emu->memory, 5, arguments, {});

        EXPECT_EQ(detail_value(details, "path"), "NULL");
    }

    TEST(MacosSyscallTrace, NonPrintableBufferFallsBackToHex)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        constexpr uint8_t bytes[] = {0x00, 0xFF, 0x10, 0x80};
        emu->memory.write_memory(data_base, bytes, sizeof(bytes));

        const std::array<uint64_t, 8> arguments{1, data_base, 4, 0, 0, 0, 0, 0};
        const auto details = sogen::describe_bsd_syscall(emu->memory, 4, arguments, {});

        EXPECT_EQ(detail_value(details, "cbuf"), "00 ff 10 80");
    }

    TEST(MacosSyscallTrace, WritevSummarisesTheIovecArray)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const uint64_t text_a = data_base + 0x100;
        const uint64_t text_b = data_base + 0x200;
        emu->memory.write_memory(text_a, "ab", 2);
        emu->memory.write_memory(text_b, "cd", 2);

        const std::array<uint64_t, 4> vector_data{text_a, 2, text_b, 2};
        emu->memory.write_memory(data_base, vector_data.data(), sizeof(vector_data));

        const std::array<uint64_t, 8> arguments{1, data_base, 2, 0, 0, 0, 0, 0};
        const auto details = sogen::describe_bsd_syscall(emu->memory, 121, arguments, {});

        EXPECT_EQ(detail_value(details, "iovp"), "[2 buffers] \"ab\" \"cd\"");
    }

    TEST(MacosSyscallTrace, UnknownSyscallNumberProducesPositionalHexArguments)
    {
        const auto emu = macos_test::make_emulator();

        const std::array<uint64_t, 8> arguments{0xDEAD, 0xBEEF, 0, 0, 0, 0, 0, 0};
        const auto details = sogen::describe_bsd_syscall(emu->memory, 0xFFFFu, arguments, {});

        ASSERT_EQ(details.size(), 2u);
        EXPECT_EQ(detail_value(details, "arg0"), "0xdead");
        EXPECT_EQ(detail_value(details, "arg1"), "0xbeef");
    }

    TEST(MacosSyscallTrace, ZeroArgumentSyscallProducesNoDetails)
    {
        const auto emu = macos_test::make_emulator();

        const std::array<uint64_t, 8> arguments{};
        EXPECT_TRUE(sogen::describe_bsd_syscall(emu->memory, 20, arguments, {}).empty());
    }

    TEST(MacosSyscallTrace, LongStringIsTruncatedAtTheConfiguredLimit)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        const std::string path(500, 'x');
        emu->memory.write_memory(data_base, path.data(), path.size() + 1);

        const std::array<uint64_t, 8> arguments{data_base, 0, 0, 0, 0, 0, 0, 0};
        const auto details = sogen::describe_bsd_syscall(emu->memory, 5, arguments, sogen::macos_trace_options{16, 32});

        EXPECT_EQ(detail_value(details, "path"), "\"xxxxxxxxxxxxxxxx\"...");
    }

    // The path and the buffer routes clamp separately; a limit honoured by one says nothing about the
    // other, and a write() of a megabyte is exactly where an unclamped read hurts.
    TEST(MacosSyscallTrace, LongBufferIsTruncatedAtTheConfiguredLimitToo)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        const std::string payload(500, 'x');
        emu->memory.write_memory(data_base, payload.data(), payload.size());

        const std::array<uint64_t, 8> arguments{1, data_base, payload.size(), 0, 0, 0, 0, 0};
        const auto details = sogen::describe_bsd_syscall(emu->memory, 4, arguments, sogen::macos_trace_options{16, 32});

        EXPECT_EQ(detail_value(details, "cbuf"), "\"xxxxxxxxxxxxxxxx\"...");
    }
}

namespace
{
    TEST(MacosMachTrapTrace, ZeroArgumentTrapProducesNoDetails)
    {
        const std::array<uint64_t, 8> arguments{0x1111, 0x2222, 0, 0, 0, 0, 0, 0};
        EXPECT_TRUE(sogen::describe_mach_trap(3, arguments).empty()) << "mach_absolute_time takes no arguments";
    }

    // Two traps with different declared counts, because a single one cannot show that the count is read
    // from the table rather than fixed. Both must declare arguments: an index whose count is zero makes
    // the row-count assertion vacuous and never runs the formatting loop at all.
    TEST(MacosMachTrapTrace, PrintsExactlyTheDeclaredArgumentCountInHex)
    {
        const std::array<uint64_t, 8> arguments{0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x10, 0x11};

        for (const uint32_t index : {uint32_t{31}, uint32_t{36}})
        {
            const auto* prototype = sogen::find_mach_trap_prototype(index);
            ASSERT_NE(prototype, nullptr) << "index " << index;
            ASSERT_GT(prototype->argument_count, 0u) << prototype->name;

            const auto details = sogen::describe_mach_trap(index, arguments);
            EXPECT_EQ(details.size(), prototype->argument_count) << prototype->name;

            for (size_t i = 0; i < details.size(); ++i)
            {
                EXPECT_EQ(details[i].label, "arg" + std::to_string(i));
                EXPECT_EQ(details[i].value, sogen::format_hex(arguments[i]));
            }
        }
    }

    TEST(MacosMachTrapTrace, UnknownTrapIndexProducesNoDetails)
    {
        const std::array<uint64_t, 8> arguments{1, 2, 3, 4, 5, 6, 7, 8};
        EXPECT_TRUE(sogen::describe_mach_trap(0xFFFFu, arguments).empty());
    }
}

namespace
{
    TEST(MacosTraceEmission, WriteSyscallEmitsDecodedArgumentDetails)
    {
        const auto emu = macos_test::make_emulator();

        std::vector<sogen::macos_trace_detail> details{};
        emu->callbacks.on_trace_detail = [&](const sogen::macos_trace_detail& detail) { details.push_back(detail); };
        emu->callbacks.on_stdout = [](std::string_view) {};

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.write_memory(data_base, "Hello, sogen!\n", 14);

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800020, // mov x0, #1
                                         0xD28001C2, // mov x2, #14
                                         0xD2800090, // mov x16, #4 (write)
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x1, data_base);

        emu->start(4);

        ASSERT_EQ(details.size(), 3u);
        EXPECT_EQ(details[0].label, "fd");
        EXPECT_EQ(details[0].value, "1");
        EXPECT_EQ(details[1].value, "\"Hello, sogen!\\n\"");
        EXPECT_EQ(details[2].value, "14");
    }

    // Deliberately a syscall that has arguments. getpid takes none, so it emits nothing whether the
    // switch is honoured or not, and asserting zero rows there would pass against any implementation.
    TEST(MacosTraceEmission, DecodeArgumentsOffSuppressesDetails)
    {
        const auto emu = macos_test::make_emulator();
        emu->trace.decode_arguments = false;

        size_t detail_count = 0;
        emu->callbacks.on_trace_detail = [&](const sogen::macos_trace_detail&) { ++detail_count; };
        emu->callbacks.on_stdout = [](std::string_view) {};

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.write_memory(data_base, "Hello, sogen!\n", 14);

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800020, // mov x0, #1
                                         0xD28001C2, // mov x2, #14
                                         0xD2800090, // mov x16, #4 (write)
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x1, data_base);

        emu->start(4);

        EXPECT_EQ(detail_count, 0u);
    }

    TEST(MacosTraceEmission, TheEmulatorsTraceSettingsBoundWhatIsEmitted)
    {
        const auto emu = macos_test::make_emulator();
        emu->trace.string_limit = 8;

        std::vector<sogen::macos_trace_detail> details{};
        emu->callbacks.on_trace_detail = [&](const sogen::macos_trace_detail& detail) { details.push_back(detail); };
        emu->callbacks.on_stdout = [](std::string_view) {};

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        const std::string payload(100, 'x');
        emu->memory.write_memory(data_base, payload.data(), payload.size());

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800020, // mov x0, #1
                                         0xD2800C82, // mov x2, #100
                                         0xD2800090, // mov x16, #4 (write)
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x1, data_base);

        emu->start(4);

        ASSERT_EQ(details.size(), 3u);
        EXPECT_EQ(details[1].value, "\"xxxxxxxx\"...");
    }

    TEST(MacosTraceEmission, NoListenerMeansNoFormattingWork)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800290, // mov x16, #20 (getpid)
                                         0xD4001001, // svc #0x80
                                     });

        EXPECT_NO_THROW(emu->start(2));
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::unhandled_cpu_exception);
    }

    TEST(MacosTraceEmission, GarbagePointerArgumentsDoNotStopEmulation)
    {
        const auto emu = macos_test::make_emulator();

        emu->callbacks.on_trace_detail = [](const sogen::macos_trace_detail&) {};

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800030, // mov x16, #1 (exit)
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::x0, 0x7FFFFFFFFFFFULL);

        EXPECT_NO_THROW(emu->start(2));
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::backend_error);
    }
}

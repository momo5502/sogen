#include <gtest/gtest.h>

#include <logger.hpp>
#include <queued_file_writer.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace sogen::test
{
    namespace
    {
        std::string read_file(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        std::string write_lines(queued_file_writer& writer, const int first, const int count)
        {
            std::string written{};
            for (int i = first; i < first + count; ++i)
            {
                const auto line = "line " + std::to_string(i) + "\n";
                writer.write(line);
                written += line;
            }

            return written;
        }

        void format_value_line(const int& value, std::string& out)
        {
            out += "value " + std::to_string(value) + "\n";
        }
    }

    TEST(QueuedFileWriterTest, WritesEveryLineInOrderAndAWaitedLineBeforeReturning)
    {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
        GTEST_SKIP() << "Requires host threads";
#endif
        const auto path = std::filesystem::temp_directory_path() / "sogen-queued-file-writer-test.log";
        auto* stream = std::fopen(path.string().c_str(), "wb");
        ASSERT_NE(stream, nullptr);

        std::string expected{};
        std::string expected_when_wait_returned{};
        std::string written_when_wait_returned{};

        {
            queued_file_writer writer{stream};
            expected += write_lines(writer, 0, 1000);
            writer.write("error\n", true);
            expected += "error\n";

            written_when_wait_returned = read_file(path);
            expected_when_wait_returned = expected;
            expected += write_lines(writer, 1000, 1000);
        }

        (void)std::fclose(stream);
        const auto written = read_file(path);
        std::filesystem::remove(path);

        EXPECT_EQ(written_when_wait_returned, expected_when_wait_returned);
        EXPECT_EQ(written, expected);
    }

    TEST(QueuedFileWriterTest, FlushReturnsOnceEveryQueuedLineIsWritten)
    {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
        GTEST_SKIP() << "Requires host threads";
#endif
        const auto path = std::filesystem::temp_directory_path() / "sogen-queued-file-writer-flush-test.log";
        auto* stream = std::fopen(path.string().c_str(), "wb");
        ASSERT_NE(stream, nullptr);

        std::string expected{};
        std::string written_when_flush_returned{};

        {
            queued_file_writer writer{stream};
            expected = write_lines(writer, 0, 1000);
            writer.flush();
            written_when_flush_returned = read_file(path);
        }

        (void)std::fclose(stream);
        std::filesystem::remove(path);

        EXPECT_EQ(written_when_flush_returned, expected);
    }

    TEST(QueuedFileWriterTest, FormatsDeferredLinesInOrderWithTheText)
    {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
        GTEST_SKIP() << "Requires host threads";
#endif
        const auto path = std::filesystem::temp_directory_path() / "sogen-queued-file-writer-deferred-test.log";
        auto* stream = std::fopen(path.string().c_str(), "wb");
        ASSERT_NE(stream, nullptr);

        std::string expected{};
        std::string expected_when_wait_returned{};
        std::string written_when_wait_returned{};

        {
            queued_file_writer writer{stream};
            for (int i = 0; i < 1000; ++i)
            {
                writer.write("text " + std::to_string(i) + "\n");
                writer.write_deferred(
                    [](const void* record, std::string& out) {
                        int value{};
                        std::memcpy(&value, record, sizeof(value));
                        out += "deferred " + std::to_string(value) + "\n";
                    },
                    &i, sizeof(i));
                expected += "text " + std::to_string(i) + "\ndeferred " + std::to_string(i) + "\n";
            }

            writer.write("error\n", true);
            expected += "error\n";
            written_when_wait_returned = read_file(path);
            expected_when_wait_returned = expected;
        }

        (void)std::fclose(stream);
        const auto written = read_file(path);
        std::filesystem::remove(path);

        EXPECT_EQ(written_when_wait_returned, expected_when_wait_returned);
        EXPECT_EQ(written, expected);
    }

    TEST(LoggerTest, DeferredLineReachesASinkFormatted)
    {
        logger log{};
        std::string seen{};
        log.set_sink([&](const color, const std::string_view message) { seen += message; });
        log.disable_output(true);

        log.print_deferred<format_value_line>(color::gray, 7);

        EXPECT_EQ(seen, "value 7\n");
    }
}

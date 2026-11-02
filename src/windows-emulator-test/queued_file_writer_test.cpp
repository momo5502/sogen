#include <gtest/gtest.h>

#include <queued_file_writer.hpp>

#include <cstdio>
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
}

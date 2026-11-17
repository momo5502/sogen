#include "emulation_test_utils.hpp"
#include <utils/finally.hpp>

#if !defined(OS_WINDOWS) && !defined(OS_EMSCRIPTEN)
#include <array>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#endif

#ifndef OS_EMSCRIPTEN
namespace sogen::test
{
    namespace
    {
        class ConsolePipeTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
#ifdef OS_WINDOWS
                this->original_input_ = GetStdHandle(STD_INPUT_HANDLE);
                ASSERT_TRUE(CreatePipe(&this->input_, &this->output_, nullptr, 0));
                ASSERT_TRUE(SetStdHandle(STD_INPUT_HANDLE, this->input_));
#else
                this->original_stream_state_ = std::cin.rdstate();
                std::cin.clear();
                std::clearerr(stdin);
                this->original_input_ = dup(STDIN_FILENO);
                ASSERT_GE(this->original_input_, 0);
                std::array<int, 2> descriptors{};
                ASSERT_EQ(0, pipe(descriptors.data()));
                this->input_ = descriptors[0];
                this->output_ = descriptors[1];
                ASSERT_EQ(STDIN_FILENO, dup2(this->input_, STDIN_FILENO));
#endif
                this->console = this->create_backend();
            }

            void TearDown() override
            {
                this->console = nullptr;
#ifdef OS_WINDOWS
                SetStdHandle(STD_INPUT_HANDLE, this->original_input_);
                if (this->input_)
                {
                    CloseHandle(this->input_);
                }
#else
                if (this->original_input_ >= 0)
                {
                    dup2(this->original_input_, STDIN_FILENO);
                    close(this->original_input_);
                }
                if (this->input_ >= 0)
                {
                    close(this->input_);
                }
                std::clearerr(stdin);
                std::cin.clear(this->original_stream_state_);
#endif
                this->close_output();
            }

            void write(const std::string_view text) const
            {
#ifdef OS_WINDOWS
                DWORD written{};
                ASSERT_TRUE(WriteFile(this->output_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr));
                ASSERT_EQ(text.size(), written);
#else
                ASSERT_EQ(static_cast<ssize_t>(text.size()), ::write(this->output_, text.data(), text.size()));
#endif
            }

            void close_output()
            {
#ifdef OS_WINDOWS
                if (this->output_)
                {
                    CloseHandle(this->output_);
                    this->output_ = nullptr;
                }
#else
                if (this->output_ >= 0)
                {
                    close(this->output_);
                    this->output_ = -1;
                }
#endif
            }

            std::unique_ptr<console_backend> console{};

            virtual std::unique_ptr<console_backend> create_backend() = 0;

#ifndef OS_WINDOWS
            void use_write_end_as_stdin()
            {
                ASSERT_EQ(STDIN_FILENO, dup2(this->output_, STDIN_FILENO));
                ASSERT_EQ(0, close(this->input_));
                this->input_ = -1;
            }
#endif

          private:
#ifdef OS_WINDOWS
            HANDLE original_input_{};
            HANDLE input_{};
            HANDLE output_{};
#else
            int original_input_{-1};
            int input_{-1};
            int output_{-1};
            std::ios_base::iostate original_stream_state_{};
#endif
        };

        class ConsoleStreamTest : public ConsolePipeTest
        {
          protected:
            std::unique_ptr<console_backend> create_backend() override
            {
                return create_stream_console_backend();
            }
        };

#ifndef OS_WINDOWS
        class PosixConsoleBackendTest : public ConsolePipeTest
        {
          protected:
            std::unique_ptr<console_backend> create_backend() override
            {
                return create_posix_console_backend();
            }
        };
#endif
    }

    TEST_F(ConsoleStreamTest, PipeDistinguishesPendingInputFromEof)
    {
        EXPECT_FALSE(this->console->try_read_input(16));
        EXPECT_FALSE(this->console->input_closed());
        this->write("abc");
        this->close_output();
        EXPECT_EQ(std::optional<std::string>{"ab"}, this->console->try_read_input(2));
        EXPECT_EQ(std::optional<std::string>{"c"}, this->console->try_read_input(16));
        EXPECT_EQ(std::optional<std::string>{""}, this->console->try_read_input(16));
        EXPECT_TRUE(this->console->input_closed());
    }

    TEST_F(ConsoleStreamTest, PipeRemainsOpenBetweenWrites)
    {
        this->write("a");
        EXPECT_EQ(std::optional<std::string>{"a"}, this->console->try_read_input(16));
        EXPECT_FALSE(this->console->try_read_input(16));
        EXPECT_FALSE(this->console->input_closed());
        this->write("b");
        EXPECT_EQ(std::optional<std::string>{"b"}, this->console->try_read_input(16));
        this->close_output();
        EXPECT_EQ(std::optional<std::string>{""}, this->console->try_read_input(16));
    }

    TEST_F(ConsoleStreamTest, BlockingAndCooperativeReadsShareBufferedInput)
    {
        this->write("abc");
        EXPECT_EQ("a", this->console->read_input(1));
        EXPECT_EQ(std::optional<std::string>{"bc"}, this->console->try_read_input(16));
        EXPECT_FALSE(this->console->try_read_input(16));
        this->close_output();
        EXPECT_EQ(std::optional<std::string>{""}, this->console->try_read_input(16));
    }

#ifndef OS_WINDOWS
    TEST_F(PosixConsoleBackendTest, ClosedStdinCompletesWithEof)
    {
        ASSERT_EQ(0, close(STDIN_FILENO));
        EXPECT_EQ(std::optional<std::string>{""}, this->console->try_read_input(16));
        EXPECT_TRUE(this->console->input_closed());
    }

    TEST_F(PosixConsoleBackendTest, PipeErrorCompletesWithEof)
    {
        ASSERT_NO_FATAL_FAILURE(this->use_write_end_as_stdin());
        EXPECT_EQ(std::optional<std::string>{""}, this->console->try_read_input(16));
        EXPECT_TRUE(this->console->input_closed());
    }

#ifdef __linux__
    TEST_F(PosixConsoleBackendTest, ReadFailureCompletesWithEof)
    {
        const auto directory = open("/", O_RDONLY | O_DIRECTORY);
        ASSERT_GE(directory, 0);
        const auto cleanup = utils::finally([directory] { close(directory); });
        ASSERT_EQ(STDIN_FILENO, dup2(directory, STDIN_FILENO));
        EXPECT_EQ(std::optional<std::string>{""}, this->console->try_read_input(16));
        EXPECT_TRUE(this->console->input_closed());
    }
#endif

    TEST_F(PosixConsoleBackendTest, OpenPipeWaitsForMoreInput)
    {
        EXPECT_FALSE(this->console->try_read_input(16));
        EXPECT_FALSE(this->console->input_closed());
        ASSERT_NO_FATAL_FAILURE(this->write("q"));
        EXPECT_EQ(std::optional<std::string>{"q"}, this->console->try_read_input(16));
        EXPECT_FALSE(this->console->try_read_input(16));
        EXPECT_FALSE(this->console->input_closed());
    }

    TEST_F(PosixConsoleBackendTest, PipeDrainsBufferedInputBeforeEof)
    {
        ASSERT_NO_FATAL_FAILURE(this->write("abc"));
        this->close_output();
        EXPECT_EQ(std::optional<std::string>{"ab"}, this->console->try_read_input(2));
        EXPECT_FALSE(this->console->input_closed());
        EXPECT_EQ(std::optional<std::string>{"c"}, this->console->try_read_input(16));
        EXPECT_EQ(std::optional<std::string>{""}, this->console->try_read_input(16));
        EXPECT_TRUE(this->console->input_closed());
    }

    TEST_F(ConsoleStreamTest, RuntimeBufferedInputIsDrainedBeforeEof)
    {
        std::string text(512, '\0');
        for (size_t i = 0; i < text.size(); ++i)
        {
            text[i] = static_cast<char>('a' + i % 26);
        }
        ASSERT_NO_FATAL_FAILURE(this->write(text));
        this->close_output();
        ASSERT_EQ(text.front(), std::cin.get());
        std::string data;
        while (data.size() < text.size() - 1)
        {
            const auto chunk = this->console->try_read_input(73);
            ASSERT_TRUE(chunk);
            ASSERT_FALSE(chunk->empty());
            data += *chunk;
        }
        EXPECT_EQ(text.substr(1), data);
        EXPECT_EQ(std::optional<std::string>{""}, this->console->try_read_input(16));
        EXPECT_TRUE(this->console->input_closed());
    }

    TEST_F(ConsoleStreamTest, RuntimeBufferedInputRemainsReadableWhilePipeIsOpen)
    {
        ASSERT_NO_FATAL_FAILURE(this->write("abc"));
        ASSERT_EQ('a', std::cin.get());
        EXPECT_EQ(std::optional<std::string>{"bc"}, this->console->try_read_input(16));
        EXPECT_FALSE(this->console->try_read_input(16));
        EXPECT_FALSE(this->console->input_closed());
        ASSERT_NO_FATAL_FAILURE(this->write("next"));
        EXPECT_EQ(std::optional<std::string>{"next"}, this->console->try_read_input(16));
        this->close_output();
        EXPECT_EQ(std::optional<std::string>{""}, this->console->try_read_input(16));
    }

    TEST_F(ConsoleStreamTest, ClosedStdinCompletesWithEof)
    {
        ASSERT_EQ(0, close(STDIN_FILENO));
        EXPECT_EQ(std::optional<std::string>{""}, this->console->try_read_input(16));
        EXPECT_TRUE(this->console->input_closed());
    }

    TEST_F(ConsoleStreamTest, UnreadablePipeInputCompletesWithEof)
    {
        ASSERT_NO_FATAL_FAILURE(this->use_write_end_as_stdin());
        EXPECT_EQ(std::optional<std::string>{""}, this->console->try_read_input(16));
        EXPECT_TRUE(this->console->input_closed());
    }

    TEST_F(ConsoleStreamTest, PollingPreservesDescriptorFlags)
    {
        const auto flags = fcntl(STDIN_FILENO, F_GETFL);
        ASSERT_GE(flags, 0);
        EXPECT_FALSE(this->console->try_read_input(16));
        EXPECT_EQ(flags, fcntl(STDIN_FILENO, F_GETFL));
        ASSERT_NO_FATAL_FAILURE(this->write("q"));
        EXPECT_EQ(std::optional<std::string>{"q"}, this->console->try_read_input(16));
        EXPECT_EQ(flags, fcntl(STDIN_FILENO, F_GETFL));
        ASSERT_EQ(0, fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK));
        EXPECT_FALSE(this->console->try_read_input(16));
        EXPECT_EQ(flags | O_NONBLOCK, fcntl(STDIN_FILENO, F_GETFL));
    }
#endif
}
#endif

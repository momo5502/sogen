#include "std_include.hpp"

#include <utils/nt_thread.hpp>

#ifdef OS_WINDOWS
namespace sogen::test
{
    namespace
    {
        class console_test_session
        {
          public:
            console_test_session()
            {
                auto* const output = GetStdHandle(STD_OUTPUT_HANDLE);
                auto* const error = GetStdHandle(STD_ERROR_HANDLE);
                DWORD mode{};
                const auto console_output = GetConsoleMode(output, &mode) != FALSE;
                const auto console_error = GetConsoleMode(error, &mode) != FALSE;
                FreeConsole();
                if (!AllocConsole())
                {
                    return;
                }
                ShowWindow(GetConsoleWindow(), SW_HIDE);
                this->input_ = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                           OPEN_EXISTING, 0, nullptr);
                this->output_ = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, 0, nullptr);
                this->available_ = this->input_ && this->output_ && SetStdHandle(STD_INPUT_HANDLE, this->input_) &&
                                   SetStdHandle(STD_OUTPUT_HANDLE, console_output ? static_cast<HANDLE>(this->output_) : output) &&
                                   SetStdHandle(STD_ERROR_HANDLE, console_error ? static_cast<HANDLE>(this->output_) : error);
            }

            [[nodiscard]] bool available() const
            {
                return this->available_;
            }

          private:
            utils::nt::handle<utils::nt::invalid_handle> input_{};
            utils::nt::handle<utils::nt::invalid_handle> output_{};
            bool available_{};
        };

        bool initialize_test_console()
        {
            static const console_test_session session{};
            return session.available();
        }

        class WindowsConsoleBackendTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                if (!initialize_test_console())
                {
                    GTEST_SKIP() << "Requires a Windows console";
                }
                DWORD mode{};
                this->input = GetStdHandle(STD_INPUT_HANDLE);
                if (!GetConsoleMode(this->input, &mode))
                {
                    GTEST_SKIP() << "Requires a native console input buffer";
                }
                this->console = create_windows_console_backend();
                ASSERT_NE(this->console, nullptr);
                this->console->set_input_mode({});
                DWORD raw_mode{};
                ASSERT_TRUE(GetConsoleMode(this->input, &raw_mode));
                ASSERT_EQ(0u, raw_mode & (ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
            }

            void queue(char16_t character, WORD key = 0, DWORD control = 0, WORD repeat = 1, bool down = true)
            {
                INPUT_RECORD record{};
                record.EventType = KEY_EVENT;
                record.Event.KeyEvent.bKeyDown = down;
                record.Event.KeyEvent.wRepeatCount = repeat;
                record.Event.KeyEvent.wVirtualKeyCode = key;
                record.Event.KeyEvent.uChar.UnicodeChar = static_cast<wchar_t>(character);
                record.Event.KeyEvent.dwControlKeyState = control;
                DWORD written{};
                ASSERT_TRUE(WriteConsoleInputW(this->input, &record, 1, &written));
                ASSERT_EQ(1u, written);
            }

            HANDLE input{};
            std::unique_ptr<console_backend> console{};
        };

        class WindowsConsoleTextTest : public WindowsConsoleBackendTest
        {
          protected:
            void SetUp() override
            {
                if (!initialize_test_console())
                {
                    GTEST_SKIP() << "Requires a Windows console";
                }
                this->original_input = GetStdHandle(STD_INPUT_HANDLE);
                this->original_output = GetStdHandle(STD_OUTPUT_HANDLE);
                this->native_input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                 OPEN_EXISTING, 0, nullptr);
                ASSERT_NE(INVALID_HANDLE_VALUE, this->native_input);
                DWORD mode{};
                ASSERT_TRUE(GetConsoleMode(this->native_input, &mode));
                this->original_mode = mode;
                ASSERT_TRUE(SetConsoleMode(this->native_input, mode | ENABLE_INSERT_MODE | ENABLE_EXTENDED_FLAGS));
                this->original_buffer = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                    OPEN_EXISTING, 0, nullptr);
                ASSERT_NE(INVALID_HANDLE_VALUE, this->original_buffer);
                this->output_buffer = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                                CONSOLE_TEXTMODE_BUFFER, nullptr);
                ASSERT_NE(INVALID_HANDLE_VALUE, this->output_buffer);
                ASSERT_TRUE(SetStdHandle(STD_INPUT_HANDLE, this->native_input));
                ASSERT_TRUE(SetStdHandle(STD_OUTPUT_HANDLE, this->output_buffer));
                ASSERT_TRUE(SetConsoleActiveScreenBuffer(this->output_buffer));
                ASSERT_TRUE(FlushConsoleInputBuffer(this->native_input));
                WindowsConsoleBackendTest::SetUp();
            }

            void TearDown() override
            {
                this->console = nullptr;
                if (this->original_mode)
                {
                    SetConsoleMode(this->native_input, *this->original_mode);
                }
                if (this->original_buffer != INVALID_HANDLE_VALUE)
                {
                    SetConsoleActiveScreenBuffer(this->original_buffer);
                }
                SetStdHandle(STD_INPUT_HANDLE, this->original_input);
                SetStdHandle(STD_OUTPUT_HANDLE, this->original_output);
                for (auto* native_handle : {this->native_input, this->original_buffer, this->output_buffer})
                {
                    if (native_handle != INVALID_HANDLE_VALUE)
                    {
                        CloseHandle(native_handle);
                    }
                }
            }

            std::optional<std::string> read_ready(const size_t length)
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                do
                {
                    if (auto data = this->console->try_read_input(length))
                    {
                        return data;
                    }
                    SwitchToThread();
                } while (std::chrono::steady_clock::now() < deadline);
                return std::nullopt;
            }

            bool cancel_ready()
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                do
                {
                    if (this->console->cancel_read_input())
                    {
                        return true;
                    }
                    SwitchToThread();
                } while (std::chrono::steady_clock::now() < deadline);
                return false;
            }

            HANDLE output_buffer{INVALID_HANDLE_VALUE};

          private:
            HANDLE original_input{};
            HANDLE original_output{};
            HANDLE native_input{INVALID_HANDLE_VALUE};
            HANDLE original_buffer{INVALID_HANDLE_VALUE};
            std::optional<DWORD> original_mode{};
        };

        class WindowsConsoleStreamCharacterTest : public WindowsConsoleTextTest
        {
          protected:
            void SetUp() override
            {
                WindowsConsoleTextTest::SetUp();
                if (testing::Test::IsSkipped() || testing::Test::HasFatalFailure())
                {
                    return;
                }
                this->console = create_stream_console_backend();
                DWORD mode{};
                ASSERT_TRUE(GetConsoleMode(this->input, &mode));
                ASSERT_TRUE(SetConsoleMode(this->input, mode & ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)));
                ASSERT_EQ(DWORD{FILE_TYPE_CHAR}, GetFileType(this->input));
            }
        };
    }

    TEST_F(WindowsConsoleTextTest, CooperativeLineReadPreservesEditingAndEcho)
    {
        this->console->set_input_mode({.processed = true, .line = true, .echo = true});
        EXPECT_FALSE(this->console->try_read_input(32));
        this->queue(u'a', 'A');
        this->queue(u'c', 'C');
        this->queue(0, VK_LEFT);
        this->queue(u'b', 'B');
        this->queue(u'\r', VK_RETURN);
        const auto data = this->read_ready(32);
        ASSERT_TRUE(data);
        EXPECT_EQ("abc\r\n", *data);
        std::array<wchar_t, 3> text{};
        DWORD count{};
        ASSERT_TRUE(ReadConsoleOutputCharacterW(this->output_buffer, text.data(), static_cast<DWORD>(text.size()), {0, 0}, &count));
        EXPECT_EQ(L"abc", std::wstring(text.data(), count));
    }

    TEST_F(WindowsConsoleTextTest, CooperativeLineReadPreservesDisabledEcho)
    {
        this->console->set_input_mode({.processed = true, .line = true, .echo = false});
        EXPECT_FALSE(this->console->try_read_input(32));
        this->queue(u'a', 'A');
        this->queue(u'b', 'B');
        this->queue(u'\b', VK_BACK);
        this->queue(u'c', 'C');
        this->queue(u'\r', VK_RETURN);
        const auto data = this->read_ready(32);
        ASSERT_TRUE(data);
        EXPECT_EQ("ac\r\n", *data);
        std::array<wchar_t, 2> text{};
        DWORD count{};
        ASSERT_TRUE(ReadConsoleOutputCharacterW(this->output_buffer, text.data(), static_cast<DWORD>(text.size()), {0, 0}, &count));
        EXPECT_EQ(L"  ", std::wstring(text.data(), count));
    }

    TEST_F(WindowsConsoleTextTest, CancelledNativeReadAllowsAnotherRead)
    {
        this->console->set_input_mode({.processed = true, .line = true, .echo = false});
        for (const auto character : {u'q', u'r', u's'})
        {
            SCOPED_TRACE(static_cast<char>(character));
            ASSERT_FALSE(this->console->try_read_input(16));
            ASSERT_TRUE(this->cancel_ready());
            ASSERT_FALSE(this->console->try_read_input(16));
            this->queue(character, static_cast<WORD>('A' + character - u'a'));
            this->queue(u'\r', VK_RETURN);
            const auto data = this->read_ready(16);
            ASSERT_TRUE(data);
            EXPECT_EQ(std::string(1, static_cast<char>(character)) + "\r\n", *data);
        }
    }

    TEST_F(WindowsConsoleTextTest, CancellationPreservesInputForHostReaders)
    {
        this->console->set_input_mode({.processed = true, .line = true, .echo = false});
        std::array<wchar_t, 16> text{};
        DWORD count{};
        BOOL read_success{};
        DWORD read_error{};
        auto* read_handle = this->input;
        utils::nt::handle<utils::nt::invalid_handle> retry_input{};
        const auto read = [&] {
            read_success = ReadConsoleW(read_handle, text.data(), static_cast<DWORD>(text.size()), &count, nullptr);
            read_error = GetLastError();
        };
        std::thread other(read);
        const auto cleanup = utils::finally([&] {
            if (!other.joinable())
            {
                return;
            }
            while (WaitForSingleObject(utils::nt::get_thread_handle(other), 0) != WAIT_OBJECT_0)
            {
                CancelIoEx(read_handle, nullptr);
                SwitchToThread();
            }
            other.join();
        });
        BOOL pending{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do
        {
            ASSERT_TRUE(GetThreadIOPendingFlag(utils::nt::get_thread_handle(other), &pending));
            if (pending)
            {
                break;
            }
            SwitchToThread();
        } while (std::chrono::steady_clock::now() < deadline);
        ASSERT_TRUE(pending);
        ASSERT_FALSE(this->console->try_read_input(16));
        ASSERT_TRUE(this->cancel_ready());
        this->queue(u'q', 'Q');
        this->queue(u'\r', VK_RETURN);
        ASSERT_EQ(WAIT_OBJECT_0, WaitForSingleObject(utils::nt::get_thread_handle(other), 5000));
        other.join();
        if (count == 0)
        {
            // Native Windows may complete another reader with zero characters when a canceled CONIN$ handle closes.
            if (!read_success)
            {
                EXPECT_EQ(DWORD{ERROR_OPERATION_ABORTED}, read_error);
            }
            retry_input = CreateFileW(L"CONIN$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            ASSERT_TRUE(retry_input);
            read_handle = retry_input;
            other = std::thread(read);
            ASSERT_EQ(WAIT_OBJECT_0, WaitForSingleObject(utils::nt::get_thread_handle(other), 5000));
            other.join();
        }
        EXPECT_TRUE(read_success);
        EXPECT_EQ(L"q\r\n", std::wstring(text.data(), count));
    }

    TEST_F(WindowsConsoleTextTest, ResetStopsActiveReaderBeforeStartingAnother)
    {
        this->console->set_input_mode({.processed = true, .line = true, .echo = false});
        ASSERT_FALSE(this->console->try_read_input(16));
        (*this->console).reset();
        this->console->set_input_mode({.processed = true, .line = true, .echo = false});
        ASSERT_FALSE(this->console->try_read_input(16));
        this->queue(u'q', 'Q');
        this->queue(u'\r', VK_RETURN);
        const auto data = this->read_ready(16);
        ASSERT_TRUE(data);
        EXPECT_EQ("q\r\n", *data);
    }

    TEST_F(WindowsConsoleTextTest, CooperativeByteReadsPreserveSplitSurrogates)
    {
        this->queue(0xD83D);
        this->queue(0xDE00);
        std::string utf8;
        for (size_t i = 0; i < 4; ++i)
        {
            const auto data = this->read_ready(1);
            ASSERT_TRUE(data);
            ASSERT_EQ(1u, data->size());
            utf8 += *data;
        }
        EXPECT_EQ("\xF0\x9F\x98\x80", utf8);
    }

    TEST_F(WindowsConsoleStreamCharacterTest, CharacterInputWaitsForDataInsteadOfReportingEof)
    {
        ASSERT_FALSE(this->console->try_read_input(16));
        EXPECT_FALSE(this->console->input_closed());
        this->queue(u'q', 'Q');
        EXPECT_EQ(std::optional<std::string>{"q"}, this->read_ready(16));
        EXPECT_FALSE(this->console->input_closed());
    }

    TEST_F(WindowsConsoleStreamCharacterTest, CancellationAllowsAnotherCharacterRead)
    {
        ASSERT_FALSE(this->console->try_read_input(16));
        ASSERT_TRUE(this->cancel_ready());
        EXPECT_FALSE(this->console->input_closed());
        ASSERT_FALSE(this->console->try_read_input(16));
        this->queue(u'q', 'Q');
        EXPECT_EQ(std::optional<std::string>{"q"}, this->read_ready(16));
    }

    TEST_F(WindowsConsoleStreamCharacterTest, ResetStopsActiveCharacterReader)
    {
        ASSERT_FALSE(this->console->try_read_input(16));
        (*this->console).reset();
        ASSERT_FALSE(this->console->try_read_input(16));
        this->queue(u'q', 'Q');
        EXPECT_EQ(std::optional<std::string>{"q"}, this->read_ready(16));
    }

    TEST_F(WindowsConsoleStreamCharacterTest, NullDeviceReportsEof)
    {
        auto* null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        ASSERT_NE(INVALID_HANDLE_VALUE, null_input);
        const auto cleanup = utils::finally([&] {
            SetStdHandle(STD_INPUT_HANDLE, this->input);
            CloseHandle(null_input);
        });
        ASSERT_TRUE(SetStdHandle(STD_INPUT_HANDLE, null_input));
        EXPECT_EQ(std::optional<std::string>{""}, this->read_ready(16));
        EXPECT_TRUE(this->console->input_closed());
    }

    TEST_F(WindowsConsoleBackendTest, PollReadsIndividualKeysWithoutEnter)
    {
        this->queue(u'q', 'Q');
        ASSERT_TRUE(this->console->input_available());
        const auto event = this->console->read_input_event(false, true);
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(console_key::character, event->key);
        EXPECT_EQ(u'q', event->character);
        EXPECT_FALSE(this->console->input_available());
    }

    TEST_F(WindowsConsoleBackendTest, DimensionsMatchHostViewport)
    {
        auto* const original = GetStdHandle(STD_OUTPUT_HANDLE);
        const utils::nt::handle<utils::nt::invalid_handle> output{
            CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
        ASSERT_TRUE(output);
        const auto restore_output = utils::finally([&] { SetStdHandle(STD_OUTPUT_HANDLE, original); });
        ASSERT_TRUE(SetStdHandle(STD_OUTPUT_HANDLE, output));
        CONSOLE_SCREEN_BUFFER_INFO info{};
        ASSERT_TRUE(GetConsoleScreenBufferInfo(output, &info));
        const auto size = this->console->get_size();
        EXPECT_EQ(info.srWindow.Right - info.srWindow.Left + 1, size.width);
        EXPECT_EQ(info.srWindow.Bottom - info.srWindow.Top + 1, size.height);
    }

    TEST_F(WindowsConsoleBackendTest, DimensionsTrackOutputViewportChanges)
    {
        auto* const original = GetStdHandle(STD_OUTPUT_HANDLE);
        auto* const output = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                       CONSOLE_TEXTMODE_BUFFER, nullptr);
        ASSERT_NE(INVALID_HANDLE_VALUE, output);
        const auto cleanup = utils::finally([&] {
            SetStdHandle(STD_OUTPUT_HANDLE, original);
            CloseHandle(output);
        });
        ASSERT_TRUE(SetStdHandle(STD_OUTPUT_HANDLE, output));
        for (const auto size : {console_size{.width = 79, .height = 17}, console_size{.width = 61, .height = 13}})
        {
            const SMALL_RECT window{
                .Left = 0, .Top = 0, .Right = static_cast<SHORT>(size.width - 1), .Bottom = static_cast<SHORT>(size.height - 1)};
            ASSERT_TRUE(SetConsoleWindowInfo(output, TRUE, &window));
            ASSERT_TRUE(SetConsoleScreenBufferSize(output, {size.width, size.height}));
            const auto reported = this->console->get_size();
            EXPECT_EQ(size.width, reported.width);
            EXPECT_EQ(size.height, reported.height);
        }
    }

    TEST_F(WindowsConsoleBackendTest, RedirectedOutputUsesDefaultDimensions)
    {
        auto* const original = GetStdHandle(STD_OUTPUT_HANDLE);
        const auto cleanup = utils::finally([&] { SetStdHandle(STD_OUTPUT_HANDLE, original); });
        ASSERT_TRUE(SetStdHandle(STD_OUTPUT_HANDLE, INVALID_HANDLE_VALUE));
        const auto size = this->console->get_size();
        EXPECT_EQ(80, size.width);
        EXPECT_EQ(25, size.height);
    }

    TEST_F(WindowsConsoleBackendTest, RedirectedInputStillUsesOutputViewport)
    {
        HANDLE pipe_input{};
        HANDLE pipe_writer{};
        ASSERT_TRUE(CreatePipe(&pipe_input, &pipe_writer, nullptr, 0));
        const auto restore_input = utils::finally([&] {
            SetStdHandle(STD_INPUT_HANDLE, this->input);
            CloseHandle(pipe_input);
            CloseHandle(pipe_writer);
        });
        auto* const original_output = GetStdHandle(STD_OUTPUT_HANDLE);
        auto* const output = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                       CONSOLE_TEXTMODE_BUFFER, nullptr);
        ASSERT_NE(INVALID_HANDLE_VALUE, output);
        const auto restore_output = utils::finally([&] {
            SetStdHandle(STD_OUTPUT_HANDLE, original_output);
            CloseHandle(output);
        });
        ASSERT_TRUE(SetStdHandle(STD_INPUT_HANDLE, pipe_input));
        ASSERT_TRUE(SetStdHandle(STD_OUTPUT_HANDLE, output));
        ASSERT_EQ(nullptr, create_windows_console_backend());
        const auto backend = create_default_console_backend();
        ASSERT_NE(nullptr, backend);
        for (const auto size : {console_size{.width = 79, .height = 17}, console_size{.width = 61, .height = 13}})
        {
            const SMALL_RECT window{
                .Left = 0, .Top = 0, .Right = static_cast<SHORT>(size.width - 1), .Bottom = static_cast<SHORT>(size.height - 1)};
            ASSERT_TRUE(SetConsoleWindowInfo(output, TRUE, &window));
            ASSERT_TRUE(SetConsoleScreenBufferSize(output, {size.width, size.height}));
            EXPECT_EQ(size.width, backend->get_size().width);
            EXPECT_EQ(size.height, backend->get_size().height);
        }
        ASSERT_TRUE(SetStdHandle(STD_OUTPUT_HANDLE, INVALID_HANDLE_VALUE));
        EXPECT_EQ(80, backend->get_size().width);
        EXPECT_EQ(25, backend->get_size().height);
    }

    TEST_F(WindowsConsoleBackendTest, ByteReadsHonorRawMode)
    {
        this->queue(u'q', 'Q');
        EXPECT_EQ("q", this->console->read_input(1));
    }

    TEST_F(WindowsConsoleBackendTest, ByteReadsHonorLineMode)
    {
        this->console->set_input_mode({.processed = true, .line = true, .echo = false});
        this->queue(u'q', 'Q');
        this->queue(u'\r', VK_RETURN);
        EXPECT_EQ("q\r\n", this->console->read_input(16));
    }

    TEST_F(WindowsConsoleBackendTest, HandlesControlNavigationAndKeyRepeat)
    {
        this->queue(0, VK_LEFT, LEFT_CTRL_PRESSED, 2);
        for (size_t i = 0; i < 2; ++i)
        {
            const auto event = this->console->read_input_event(false, true);
            ASSERT_TRUE(event.has_value());
            EXPECT_EQ(console_key::left, event->key);
            EXPECT_TRUE(event->control);
        }
        EXPECT_FALSE(this->console->input_available());
    }

    TEST_F(WindowsConsoleBackendTest, PreservesControlOnCharacterKeys)
    {
        struct character_key
        {
            char16_t character;
            WORD key;
            console_key expected_key;
            char16_t expected_character;
        };

        constexpr std::array keys{
            character_key{.character = u'\r', .key = VK_RETURN, .expected_key = console_key::enter, .expected_character = u'\r'},
            character_key{.character = u'\n', .key = VK_RETURN, .expected_key = console_key::enter, .expected_character = u'\r'},
            character_key{.character = u'\t', .key = VK_TAB, .expected_key = console_key::tab, .expected_character = u'\t'},
            character_key{.character = u'\b', .key = VK_BACK, .expected_key = console_key::backspace, .expected_character = u'\b'},
            character_key{.character = 0x7F, .key = VK_BACK, .expected_key = console_key::backspace, .expected_character = u'\b'},
            character_key{.character = u'.', .key = VK_OEM_PERIOD, .expected_key = console_key::character, .expected_character = u'.'},
            character_key{.character = u'\u03BB', .key = 'L', .expected_key = console_key::character, .expected_character = u'\u03BB'},
            character_key{.character = u'\x1B', .key = VK_ESCAPE, .expected_key = console_key::escape, .expected_character = 0},
        };
        for (const auto control : std::array<DWORD, 3>{0, LEFT_CTRL_PRESSED, RIGHT_CTRL_PRESSED})
        {
            for (const auto& key : keys)
            {
                this->queue(key.character, key.key, control, 2);
                for (size_t i = 0; i < 2; ++i)
                {
                    ASSERT_TRUE(this->console->input_available());
                    for (const bool remove : {false, true})
                    {
                        const auto event = this->console->read_input_event(false, remove);
                        ASSERT_TRUE(event.has_value());
                        EXPECT_EQ(key.expected_key, event->key);
                        EXPECT_EQ(key.expected_character, event->character);
                        EXPECT_EQ(control != 0, event->control);
                    }
                }
                EXPECT_FALSE(this->console->input_available());
            }
        }
    }

    TEST_F(WindowsConsoleBackendTest, AltGrTextDoesNotBecomeControlShortcuts)
    {
        for (const auto state :
             std::array<DWORD, 3>{0, LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED, LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED | SHIFT_PRESSED})
        {
            for (const auto character : std::u16string_view{u"\\@{}[]|\u20AC"})
            {
                this->queue(character, VK_OEM_5, state, 2);
                for (size_t i = 0; i < 2; ++i)
                {
                    for (const bool remove : {false, true})
                    {
                        const auto event = this->console->read_input_event(false, remove);
                        ASSERT_TRUE(event.has_value());
                        EXPECT_EQ(console_key::character, event->key);
                        EXPECT_EQ(character, event->character);
                        EXPECT_FALSE(event->control);
                    }
                }
            }
            EXPECT_FALSE(this->console->input_available());
        }
    }

    TEST_F(WindowsConsoleBackendTest, SynthesizedTextPreservesClipboardPaths)
    {
        for (const auto key : std::array<WORD, 2>{0, VK_PACKET})
        {
            for (const auto state : std::array<DWORD, 4>{0, LEFT_CTRL_PRESSED, RIGHT_CTRL_PRESSED, LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED})
            {
                for (const auto character : std::u16string_view{u"C:\\Users\\test\\hello.py"})
                {
                    this->queue(character, key, state);
                    for (const bool remove : {false, true})
                    {
                        const auto event = this->console->read_input_event(false, remove);
                        ASSERT_TRUE(event.has_value());
                        EXPECT_EQ(console_key::character, event->key);
                        EXPECT_EQ(character, event->character);
                        EXPECT_FALSE(event->control);
                    }
                }
                EXPECT_FALSE(this->console->input_available());
            }
        }
    }

    TEST_F(WindowsConsoleBackendTest, RealControlBackslashRetainsControl)
    {
        for (const auto state :
             std::array<DWORD, 3>{LEFT_CTRL_PRESSED, RIGHT_CTRL_PRESSED, LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | RIGHT_ALT_PRESSED})
        {
            this->queue(u'\\', VK_OEM_5, state);
            const auto event = this->console->read_input_event(false, true);
            ASSERT_TRUE(event.has_value());
            EXPECT_EQ(u'\\', event->character);
            EXPECT_TRUE(event->control);
        }
    }

    TEST_F(WindowsConsoleBackendTest, PreservesControlOnDeleteFamilyKeys)
    {
        constexpr std::array keys{
            std::pair{VK_INSERT, console_key::insert},
            std::pair{VK_DELETE, console_key::delete_key},
            std::pair{VK_PRIOR, console_key::page_up},
            std::pair{VK_NEXT, console_key::page_down},
        };
        for (const auto control : std::array<DWORD, 3>{0, LEFT_CTRL_PRESSED, RIGHT_CTRL_PRESSED})
        {
            for (const auto& [key, expected_key] : keys)
            {
                this->queue(0, static_cast<WORD>(key), control, 2);
                for (size_t i = 0; i < 2; ++i)
                {
                    ASSERT_TRUE(this->console->input_available());
                    for (const bool remove : {false, true})
                    {
                        const auto event = this->console->read_input_event(false, remove);
                        ASSERT_TRUE(event.has_value());
                        EXPECT_EQ(expected_key, event->key);
                        EXPECT_EQ(control != 0, event->control);
                    }
                }
                EXPECT_FALSE(this->console->input_available());
            }
        }
    }

    TEST_F(WindowsConsoleBackendTest, PeekAndByteReadsShareInput)
    {
        this->queue(u'\x1B', VK_ESCAPE);
        this->queue(u'q', 'Q');
        const auto peek = this->console->read_input_event(false, false);
        ASSERT_TRUE(peek.has_value());
        EXPECT_EQ(console_key::escape, peek->key);
        const auto event = this->console->read_input_event(false, true);
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(console_key::escape, event->key);
        EXPECT_EQ("q", this->console->read_input(1));
    }

    TEST_F(WindowsConsoleBackendTest, IgnoresKeyReleaseAndModifierOnlyRecords)
    {
        this->queue(u'q', 'Q', 0, 1, false);
        this->queue(0, VK_SHIFT);
        EXPECT_FALSE(this->console->input_available());
        this->queue(u'r', 'R');
        const auto event = this->console->read_input_event(false, true);
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(u'r', event->character);
    }

    TEST_F(WindowsConsoleBackendTest, PreservesSurrogatesAcrossPolls)
    {
        this->queue(0xD83D);
        EXPECT_FALSE(this->console->input_available());
        this->queue(0xDE00);
        ASSERT_TRUE(this->console->input_available());
        EXPECT_EQ("\xF0\x9F\x98\x80", this->console->read_input(4));
    }

    TEST_F(WindowsConsoleBackendTest, ResetAndDestructionRestoreHostMode)
    {
        this->console = nullptr;
        DWORD original{};
        ASSERT_TRUE(GetConsoleMode(this->input, &original));
        this->console = create_windows_console_backend();
        this->console->set_input_mode({});
        (*this->console).reset();
        DWORD restored{};
        ASSERT_TRUE(GetConsoleMode(this->input, &restored));
        EXPECT_EQ(original, restored);
        this->console->set_input_mode({});
        this->console = nullptr;
        ASSERT_TRUE(GetConsoleMode(this->input, &restored));
        EXPECT_EQ(original, restored);
    }
}
#endif

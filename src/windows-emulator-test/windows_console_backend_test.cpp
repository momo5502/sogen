#include "emulation_test_utils.hpp"

#ifdef OS_WINDOWS
namespace sogen::test
{
    namespace
    {
        class WindowsConsoleBackendTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
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

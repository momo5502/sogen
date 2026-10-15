#include "emulation_test_utils.hpp"
#include <utils/finally.hpp>

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

    TEST_F(WindowsConsoleBackendTest, DimensionsMatchHostViewport)
    {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        ASSERT_TRUE(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info));
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
            const SMALL_RECT window{0, 0, static_cast<SHORT>(size.width - 1), static_cast<SHORT>(size.height - 1)};
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

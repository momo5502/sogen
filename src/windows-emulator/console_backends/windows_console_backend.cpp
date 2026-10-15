#include "../std_include.hpp"
#include "buffered_console_backend.hpp"

#include <limits>

#ifdef OS_WINDOWS
namespace sogen
{
    namespace
    {
        std::u16string_view console_key_sequence(const WORD key, const bool control)
        {
            switch (key)
            {
            case VK_UP:
                return control ? u"\x1B[1;5A" : u"\x1B[A";
            case VK_DOWN:
                return control ? u"\x1B[1;5B" : u"\x1B[B";
            case VK_RIGHT:
                return control ? u"\x1B[1;5C" : u"\x1B[C";
            case VK_LEFT:
                return control ? u"\x1B[1;5D" : u"\x1B[D";
            case VK_HOME:
                return control ? u"\x1B[1;5H" : u"\x1B[H";
            case VK_END:
                return control ? u"\x1B[1;5F" : u"\x1B[F";
            case VK_INSERT:
                return control ? u"\x1B[2;5~" : u"\x1B[2~";
            case VK_DELETE:
                return control ? u"\x1B[3;5~" : u"\x1B[3~";
            case VK_PRIOR:
                return control ? u"\x1B[5;5~" : u"\x1B[5~";
            case VK_NEXT:
                return control ? u"\x1B[6;5~" : u"\x1B[6~";
            default:
                return {};
            }
        }

        class windows_console_backend final : public buffered_console_backend
        {
          public:
            windows_console_backend(HANDLE input, DWORD mode)
                : input_handle_(input),
                  original_mode_(mode)
            {
            }

            ~windows_console_backend() override
            {
                this->restore_terminal();
            }

            console_size get_size() const override
            {
                CONSOLE_SCREEN_BUFFER_INFO info{};
                if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
                {
                    return {};
                }
                const auto width = info.srWindow.Right - info.srWindow.Left + 1;
                const auto height = info.srWindow.Bottom - info.srWindow.Top + 1;
                constexpr auto maximum = std::numeric_limits<int16_t>::max();
                if (width <= 0 || height <= 0 || width > maximum || height > maximum)
                {
                    return {};
                }
                return {.width = static_cast<int16_t>(width), .height = static_cast<int16_t>(height)};
            }

            std::string read_input(const size_t length) override
            {
                if (length == 0)
                {
                    return {};
                }
                while (this->input_.empty())
                {
                    std::array<wchar_t, 256> text{};
                    DWORD count{};
                    if (!ReadConsoleW(this->input_handle_, text.data(), static_cast<DWORD>(std::min(length, text.size())), &count,
                                      nullptr) ||
                        count == 0)
                    {
                        return {};
                    }
                    this->buffer_text(std::u16string{text.begin(), text.begin() + count});
                }
                return buffered_console_backend::read_input(length);
            }

            void set_input_mode(const console_input_mode& mode) override
            {
                auto configured = this->original_mode_ & ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
                if (mode.processed)
                {
                    configured |= ENABLE_PROCESSED_INPUT;
                }
                if (mode.line)
                {
                    configured |= ENABLE_LINE_INPUT;
                    if (mode.echo)
                    {
                        configured |= ENABLE_ECHO_INPUT;
                    }
                }
                if (SetConsoleMode(this->input_handle_, configured))
                {
                    this->terminal_modified_ = true;
                }
            }

            void reset() override
            {
                this->restore_terminal();
                this->pending_surrogate_ = 0;
                buffered_console_backend::reset();
            }

          protected:
            void refill(const int timeout_ms) override
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
                while (this->input_.empty())
                {
                    const auto now = std::chrono::steady_clock::now();
                    const auto timeout = timeout_ms < 0
                                             ? INFINITE
                                             : static_cast<DWORD>(std::max<int64_t>(
                                                   0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
                    if (WaitForSingleObject(this->input_handle_, timeout) != WAIT_OBJECT_0)
                    {
                        return;
                    }

                    std::array<INPUT_RECORD, 64> records{};
                    DWORD count{};
                    if (!ReadConsoleInputW(this->input_handle_, records.data(), static_cast<DWORD>(records.size()), &count))
                    {
                        return;
                    }

                    std::u16string text;
                    for (const auto& record : std::span(records).first(count))
                    {
                        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
                        {
                            continue;
                        }
                        const auto& key = record.Event.KeyEvent;
                        const auto control = (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                        const auto synthesized = key.wVirtualKeyCode == 0 || key.wVirtualKeyCode == VK_PACKET;
                        const auto alt_gr = key.uChar.UnicodeChar >= L' ' && key.uChar.UnicodeChar != L'\x7F' &&
                                            (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | RIGHT_ALT_PRESSED)) ==
                                                (LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED);
                        for (WORD i = 0; i < key.wRepeatCount; ++i)
                        {
                            if (key.uChar.UnicodeChar)
                            {
                                if (control && !synthesized && !alt_gr)
                                {
                                    const auto sequence = "\x1B[" + std::to_string(static_cast<uint16_t>(key.uChar.UnicodeChar)) + ";5u";
                                    text.append(sequence.begin(), sequence.end());
                                }
                                else
                                {
                                    text.push_back(static_cast<char16_t>(key.uChar.UnicodeChar));
                                }
                            }
                            else
                            {
                                text.append(console_key_sequence(key.wVirtualKeyCode, control));
                            }
                        }
                    }
                    this->buffer_text(std::move(text));
                    if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline)
                    {
                        return;
                    }
                }
            }

          private:
            void buffer_text(std::u16string text)
            {
                if (this->pending_surrogate_)
                {
                    text.insert(text.begin(), this->pending_surrogate_);
                    this->pending_surrogate_ = 0;
                }
                if (!text.empty() && text.back() >= 0xD800 && text.back() <= 0xDBFF)
                {
                    this->pending_surrogate_ = text.back();
                    text.pop_back();
                }
                const auto utf8 = u16_to_u8(text);
                this->input_.insert(this->input_.end(), utf8.begin(), utf8.end());
            }

            void restore_terminal()
            {
                if (this->terminal_modified_)
                {
                    SetConsoleMode(this->input_handle_, this->original_mode_);
                    this->terminal_modified_ = false;
                }
            }

            HANDLE input_handle_{};
            DWORD original_mode_{};
            bool terminal_modified_{};
            char16_t pending_surrogate_{};
        };
    }

    std::unique_ptr<console_backend> create_windows_console_backend()
    {
        auto* const input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode{};
        if (!GetConsoleMode(input, &mode))
        {
            return {};
        }
        return std::make_unique<windows_console_backend>(input, mode);
    }
}
#endif

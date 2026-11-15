#include "../std_include.hpp"
#include "buffered_console_backend.hpp"

#include <chrono>
#include <iostream>
#include <thread>
#ifdef OS_WINDOWS
#include <system_error>
#include <utils/nt_handle.hpp>
#include <utils/nt_thread.hpp>
#elif !defined(OS_EMSCRIPTEN)
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <utils/finally.hpp>
#endif

namespace sogen
{
    namespace
    {
        class stream_console_backend final : public buffered_console_backend
        {
          public:
#ifdef OS_WINDOWS
            ~stream_console_backend() override
            {
                this->stop_reader();
            }

            bool cancel_read_input() override
            {
                if (!this->reader_.joinable())
                {
                    return true;
                }
                const auto canceled = this->console_reader_ ? CancelIoEx(this->reader_handle_, nullptr)
                                                            : CancelSynchronousIo(utils::nt::get_thread_handle(this->reader_));
                if (!canceled)
                {
                    const auto error = GetLastError();
                    if (error != ERROR_NOT_FOUND)
                    {
                        throw std::system_error(static_cast<int>(error), std::system_category(),
                                                this->console_reader_ ? "CancelIoEx" : "CancelSynchronousIo");
                    }
                }
                if (WaitForSingleObject(utils::nt::get_thread_handle(this->reader_), 0) != WAIT_OBJECT_0)
                {
                    return false;
                }
                this->reader_.join();
                this->collect_read(true);
                if (this->console_reader_)
                {
                    this->reader_handle_ = INVALID_HANDLE_VALUE;
                }
                return true;
            }

            void reset() override
            {
                this->stop_reader();
                this->reader_handle_ = INVALID_HANDLE_VALUE;
                buffered_console_backend::reset();
            }
#endif

            void set_input_mode(const console_input_mode& /*mode*/) override
            {
            }

          protected:
            void refill(const int timeout_ms) override
            {
                if (!this->input_.empty())
                {
                    return;
                }

#ifdef OS_EMSCRIPTEN
                if (timeout_ms < 0)
                {
                    this->read_one();
                    return;
                }
#endif

                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
                do
                {
                    this->poll_input();
                    if (!this->input_.empty() || this->input_closed_)
                    {
                        return;
                    }

                    if (timeout_ms == 0)
                    {
                        return;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while (timeout_ms < 0 || std::chrono::steady_clock::now() < deadline);
            }

          private:
            void poll_input()
            {
                if (this->input_closed_)
                {
                    return;
                }
#if !defined(OS_WINDOWS) && !defined(OS_EMSCRIPTEN)
                this->read_posix_input();
#else
                const auto available = std::cin.rdbuf()->in_avail();
                if (available > 0)
                {
                    this->read_available();
                    return;
                }
                if (available < 0 || std::cin.eof())
                {
                    this->input_closed_ = true;
                    return;
                }
#ifdef OS_WINDOWS
                auto* const input = GetStdHandle(STD_INPUT_HANDLE);
                const auto type = GetFileType(input);
                DWORD amount = 256;
                if (type == FILE_TYPE_PIPE)
                {
                    if (!PeekNamedPipe(input, nullptr, 0, nullptr, &amount, nullptr))
                    {
                        this->input_closed_ = true;
                        return;
                    }
                    if (amount == 0)
                    {
                        return;
                    }
                }
                else if (type == FILE_TYPE_CHAR)
                {
                    this->poll_character_input(input);
                    return;
                }
                else if (type != FILE_TYPE_DISK)
                {
                    this->input_closed_ = true;
                    return;
                }
                std::array<uint8_t, 256> data{};
                DWORD count{};
                if (!ReadFile(input, data.data(), std::min<DWORD>(amount, static_cast<DWORD>(data.size())), &count, nullptr) || count == 0)
                {
                    this->input_closed_ = true;
                    return;
                }
                this->input_.insert(this->input_.end(), data.begin(), data.begin() + count);
#endif
#endif
            }

#if !defined(OS_WINDOWS) && !defined(OS_EMSCRIPTEN)
            void read_posix_input()
            {
                const auto flags = fcntl(STDIN_FILENO, F_GETFL);
                if (flags < 0 && errno != EBADF)
                {
                    this->input_closed_ = true;
                    return;
                }
                const auto change_flags = flags >= 0 && (flags & O_NONBLOCK) == 0;
                if (change_flags && fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
                {
                    this->input_closed_ = true;
                    return;
                }
                const auto restore_flags = utils::finally([&] {
                    if (change_flags)
                    {
                        fcntl(STDIN_FILENO, F_SETFL, flags);
                    }
                });
                for (size_t count = 0; count < 256;)
                {
                    auto character = std::char_traits<char>::eof();
                    errno = 0;
                    try
                    {
                        character = std::cin.rdbuf()->sbumpc();
                    }
                    catch (const std::ios_base::failure&)
                    {
                        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                        {
                            this->input_closed_ = true;
                            return;
                        }
                    }
                    if (character != std::char_traits<char>::eof())
                    {
                        this->input_.push_back(static_cast<uint8_t>(character));
                        ++count;
                        continue;
                    }
                    const auto error = errno;
                    if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK)
                    {
                        std::clearerr(stdin);
                        if (error == EINTR)
                        {
                            continue;
                        }
                    }
                    else
                    {
                        this->input_closed_ = true;
                    }
                    return;
                }
            }
#endif

#ifdef OS_WINDOWS
            void poll_character_input(HANDLE input)
            {
                if (this->reader_.joinable())
                {
                    if (WaitForSingleObject(utils::nt::get_thread_handle(this->reader_), 0) != WAIT_OBJECT_0)
                    {
                        return;
                    }
                    this->reader_.join();
                    this->collect_read();
                    return;
                }
                if (!this->reader_handle_)
                {
                    DWORD mode{};
                    this->console_reader_ = GetConsoleMode(input, &mode) != FALSE;
                    if (this->console_reader_)
                    {
                        this->reader_handle_ =
                            CreateFileW(L"CONIN$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                    }
                    else
                    {
                        HANDLE duplicate{};
                        if (DuplicateHandle(GetCurrentProcess(), input, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
                        {
                            this->reader_handle_ = duplicate;
                        }
                    }
                    if (!this->reader_handle_)
                    {
                        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Opening character reader");
                    }
                    DCB configuration{};
                    configuration.DCBlength = sizeof(configuration);
                    this->serial_reader_ = GetCommState(this->reader_handle_, &configuration) != FALSE;
                }
                this->reader_ = std::thread([this] {
                    this->read_count_ = 0;
                    const auto length = this->serial_reader_ ? 1u : static_cast<DWORD>(this->read_buffer_.size());
                    this->read_error_ = ReadFile(this->reader_handle_, this->read_buffer_.data(), length, &this->read_count_, nullptr)
                                            ? ERROR_SUCCESS
                                            : GetLastError();
                    if (this->serial_reader_ && this->read_count_)
                    {
                        COMSTAT status{};
                        DWORD errors{};
                        if (ClearCommError(this->reader_handle_, &errors, &status) && status.cbInQue)
                        {
                            const auto remaining = static_cast<DWORD>(this->read_buffer_.size()) - this->read_count_;
                            DWORD count{};
                            ReadFile(this->reader_handle_, this->read_buffer_.data() + this->read_count_,
                                     std::min(status.cbInQue, remaining), &count, nullptr);
                            this->read_count_ += count;
                        }
                    }
                });
            }

            void collect_read(const bool canceled = false)
            {
                if (this->read_count_)
                {
                    this->input_.insert(this->input_.end(), this->read_buffer_.begin(), this->read_buffer_.begin() + this->read_count_);
                }
                else if (!canceled && this->read_error_ != ERROR_OPERATION_ABORTED &&
                         (this->read_error_ != ERROR_SUCCESS || !this->serial_reader_))
                {
                    this->input_closed_ = true;
                }
            }

            void stop_reader()
            {
                while (!this->cancel_read_input())
                {
                    WaitForSingleObject(utils::nt::get_thread_handle(this->reader_), 1);
                }
            }

            utils::nt::handle<utils::nt::invalid_handle> reader_handle_{};
            std::thread reader_{};
            bool console_reader_{};
            bool serial_reader_{};
            std::array<uint8_t, 256> read_buffer_{};
            DWORD read_count_{};
            DWORD read_error_{};
#endif

#ifdef OS_EMSCRIPTEN
            void read_one()
            {
                const auto character = std::cin.get();
                if (character != std::char_traits<char>::eof())
                {
                    this->input_.push_back(static_cast<uint8_t>(character));
                    this->read_available();
                }
                else
                {
                    this->input_closed_ = true;
                }
            }
#endif

            void read_available()
            {
                while (std::cin.rdbuf()->in_avail() > 0)
                {
                    const auto character = std::cin.get();
                    if (character == std::char_traits<char>::eof())
                    {
                        return;
                    }

                    this->input_.push_back(static_cast<uint8_t>(character));
                }
            }
        };
    }

    std::unique_ptr<console_backend> create_stream_console_backend()
    {
        return std::make_unique<stream_console_backend>();
    }

} // namespace sogen

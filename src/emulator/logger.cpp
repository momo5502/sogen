#include "logger.hpp"

#include <utils/finally.hpp>
#include <utils/win.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif

namespace sogen
{

    namespace
    {
#ifdef _WIN32
#define COLOR(win, posix, web) win
        using color_type = WORD;
#elif defined(__EMSCRIPTEN__) && !defined(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS)
#define COLOR(win, posix, web) web
        using color_type = const char*;
#else
#define COLOR(win, posix, web) posix
        using color_type = const char*;
#endif

        color_type get_reset_color()
        {
            return COLOR(7, "\033[0m", "</span>");
        }

        color_type get_color_type(const color c)
        {
            using enum color;

            switch (c)
            {
            case black:
                return COLOR(0x8, "\033[0;90m", "<span class=\"terminal-black\">");
            case red:
                return COLOR(0xC, "\033[0;91m", "<span class=\"terminal-red\">");
            case green:
                return COLOR(0xA, "\033[0;92m", "<span class=\"terminal-green\">");
            case yellow:
                return COLOR(0xE, "\033[0;93m", "<span class=\"terminal-yellow\">");
            case blue:
                return COLOR(0x9, "\033[0;94m", "<span class=\"terminal-blue\">");
            case cyan:
                return COLOR(0xB, "\033[0;96m", "<span class=\"terminal-cyan\">");
            case pink:
                return COLOR(0xD, "\033[0;95m", "<span class=\"terminal-pink\">");
            case white:
                return COLOR(0xF, "\033[0;97m", "<span class=\"terminal-white\">");
            case dark_gray:
                return COLOR(0x8, "\033[0;90m", "<span class=\"terminal-dark-gray\">");
            case gray:
            default:
                return get_reset_color();
            }
        }

#ifdef _WIN32
        HANDLE get_console_handle()
        {
            return GetStdHandle(STD_OUTPUT_HANDLE);
        }
#endif

        // Output redirected to a file or pipe gets no colors, and flushing it after every line would cost a host
        // write per line, which verbose runs make thousands of times a second.
        bool stdout_is_terminal()
        {
#ifdef _WIN32
            return GetFileType(get_console_handle()) == FILE_TYPE_CHAR;
#elif defined(__EMSCRIPTEN__)
            return true;
#else
            return isatty(fileno(stdout)) != 0;
#endif
        }

        constexpr auto flush_interval = std::chrono::milliseconds(50);

        // A redirected line that is not flushed when it is written would wait in the stdio buffer for a later line, which
        // never comes when the run hangs, crashes or is terminated, and those are the runs a log is read for. This host
        // thread flushes such a line within an interval or two, whatever is logged after it.
        class unflushed_output_flusher
        {
          public:
            unflushed_output_flusher() = default;

            ~unflushed_output_flusher()
            {
                {
                    const std::scoped_lock lock(this->mutex_);
                    this->running_ = false;
                }
                this->changed_.notify_one();
                this->thread_.join();
            }

            unflushed_output_flusher(const unflushed_output_flusher&) = delete;
            unflushed_output_flusher& operator=(const unflushed_output_flusher&) = delete;
            unflushed_output_flusher(unflushed_output_flusher&&) = delete;
            unflushed_output_flusher& operator=(unflushed_output_flusher&&) = delete;

            void line_left_unflushed()
            {
                {
                    const std::scoped_lock lock(this->mutex_);
                    if (this->pending_)
                    {
                        return;
                    }
                    this->pending_ = true;
                }
                this->changed_.notify_one();
            }

          private:
            void run()
            {
                std::unique_lock lock(this->mutex_);
                while (true)
                {
                    this->changed_.wait(lock, [this] { return this->pending_ || !this->running_; });
                    if (!this->running_)
                    {
                        return;
                    }

                    // Later lines in the interval share the flush.
                    this->changed_.wait_for(lock, flush_interval, [this] { return !this->running_; });
                    this->pending_ = false;
                    lock.unlock();
                    (void)fflush(stdout);
                    lock.lock();
                }
            }

            std::mutex mutex_{};
            std::condition_variable changed_{};
            bool pending_{false};
            bool running_{true};
            std::thread thread_{[this] { this->run(); }};
        };

        void flush_soon()
        {
            static unflushed_output_flusher flusher{};
            flusher.line_left_unflushed();
        }

        void set_color(const color_type color)
        {
#ifdef _WIN32
            SetConsoleTextAttribute(get_console_handle(), color);
#else
            printf("%s", color);
#endif
        }

        void reset_color()
        {
            (void)fflush(stdout);
            set_color(get_reset_color());
            (void)fflush(stdout);
        }

        int format_internal(const char* message, std::span<char> buffer, va_list* ap)
        {
            return vsnprintf(buffer.data(), buffer.size(), message, *ap);
        }

        std::string_view format(const char* message, std::string& reserve_buffer, va_list* ap1, va_list* ap2)
        {
            thread_local std::array<char, 0x1000> buffer{};

            auto count = format_internal(message, buffer, ap1);

            if (count < 0)
            {
                return {};
            }

            if (static_cast<size_t>(count) < buffer.size())
            {
                return {buffer.data(), static_cast<size_t>(count)};
            }

            reserve_buffer.resize(count + 1);
            count = format_internal(message, reserve_buffer, ap2);

            if (count < 0)
            {
                return {};
            }

            return {reserve_buffer.data(), std::min(static_cast<size_t>(count), reserve_buffer.size() - 1)};
        }

#define format_to_string(msg, str)                 \
    std::string buf{};                             \
    va_list ap1;                                   \
    va_list ap2;                                   \
    va_start(ap1, msg);                            \
    va_start(ap2, msg);                            \
    const auto str = format(msg, buf, &ap1, &ap2); \
    va_end(ap2);                                   \
    va_end(ap1)

        void print_colored(const std::string_view& line, const color_type base_color)
        {
            const auto _ = utils::finally(&reset_color);
            set_color(base_color);
            (void)fwrite(line.data(), 1, line.size(), stdout);
        }
    }

    logger::logger()
        : terminal_(stdout_is_terminal())
    {
#ifdef _WIN32
        old_cp = GetConsoleOutputCP();
        SetConsoleOutputCP(CP_UTF8);
#endif
    }

#ifdef _WIN32
    logger::~logger()
    {
        SetConsoleOutputCP(old_cp);
    }
#endif

    void logger::print_message(const color c, const std::string_view message, const bool force) const
    {
        const std::scoped_lock lock(this->print_mutex_);

        // Sinks observe all log activity, regardless of disable_output_. That lets
        // consumers capture a full structured log even when they've silenced the
        // terminal (e.g. --silent, or a Python wrapper capturing via callback).
        this->sink_(c, message);

        if (this->silent_ || (!force && this->disable_output_))
        {
            return;
        }

        if (this->terminal_)
        {
            print_colored(message, get_color_type(c));
            return;
        }

        (void)fwrite(message.data(), 1, message.size(), stdout);

        const auto now = std::chrono::steady_clock::now();
        if (force || c == color::red || now - this->last_flush_ >= flush_interval)
        {
            (void)fflush(stdout);
            this->last_flush_ = now;
        }
        else
        {
            flush_soon();
        }
    }

    void logger::print(const color c, const std::string_view message)
    {
        this->print_message(c, message);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp,modernize-avoid-variadic-functions)
    void logger::print(const color c, const char* message, ...)
    {
        format_to_string(message, data);
        this->print_message(c, data);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp,modernize-avoid-variadic-functions)
    void logger::force_print(const color c, const char* message, ...)
    {
        format_to_string(message, data);
        this->print_message(c, data, true);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp,modernize-avoid-variadic-functions)
    void logger::info(const char* message, ...) const
    {
        format_to_string(message, data);
        this->print_message(color::cyan, data);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp,modernize-avoid-variadic-functions)
    void logger::warn(const char* message, ...) const
    {
        format_to_string(message, data);
        this->print_message(color::yellow, data);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp,modernize-avoid-variadic-functions)
    void logger::error(const char* message, ...) const
    {
        format_to_string(message, data);
        this->print_message(color::red, data, true);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp,modernize-avoid-variadic-functions)
    void logger::success(const char* message, ...) const
    {
        format_to_string(message, data);
        this->print_message(color::green, data);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp,modernize-avoid-variadic-functions)
    void logger::log(const char* message, ...) const
    {
        format_to_string(message, data);
        this->print_message(color::gray, data);
    }

} // namespace sogen

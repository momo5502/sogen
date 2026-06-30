#pragma once

#include "std_include.hpp"

namespace sogen
{

    class linux_logger
    {
      public:
        linux_logger() = default;

        // NOLINTNEXTLINE(cert-dcl50-cpp,modernize-avoid-variadic-functions)
        void info(const char* fmt, ...) const
        {
            if (this->disable_output_)
            {
                return;
            }

            va_list ap; // NOLINT(cppcoreguidelines-init-variables)
            va_start(ap, fmt);
            fprintf(stdout, "[INFO] ");
            vfprintf(stdout, fmt, ap);
            va_end(ap);
        }

        // NOLINTNEXTLINE(cert-dcl50-cpp,modernize-avoid-variadic-functions)
        void warn(const char* fmt, ...) const
        {
            if (this->disable_output_)
            {
                return;
            }

            va_list ap; // NOLINT(cppcoreguidelines-init-variables)
            va_start(ap, fmt);
            fprintf(stderr, "[WARN] ");
            vfprintf(stderr, fmt, ap);
            va_end(ap);
        }

        // Errors are always printed, regardless of disable_output_.
        // NOLINTNEXTLINE(cert-dcl50-cpp,modernize-avoid-variadic-functions)
        static void error(const char* fmt, ...)
        {
            va_list ap; // NOLINT(cppcoreguidelines-init-variables)
            va_start(ap, fmt);
            fprintf(stderr, "[ERROR] ");
            vfprintf(stderr, fmt, ap);
            va_end(ap);
        }

        // NOLINTNEXTLINE(cert-dcl50-cpp,modernize-avoid-variadic-functions)
        void print(const char* fmt, ...) const
        {
            if (this->disable_output_)
            {
                return;
            }

            va_list ap; // NOLINT(cppcoreguidelines-init-variables)
            va_start(ap, fmt);
            vfprintf(stdout, fmt, ap);
            va_end(ap);
        }

        void disable_output(const bool value)
        {
            this->disable_output_ = value;
        }

        bool is_output_disabled() const
        {
            return this->disable_output_;
        }

      private:
        bool disable_output_{false};
    };

} // namespace sogen

#pragma once

#include "std_include.hpp"

class linux_logger
{
  public:
    linux_logger() = default;

    // NOLINTNEXTLINE(cert-dcl50-cpp)
    void info(const char* fmt, ...) const
    {
        if (this->disable_output_)
        {
            return;
        }

        va_list ap;
        va_start(ap, fmt);
        fprintf(stdout, "[INFO] ");
        vfprintf(stdout, fmt, ap);
        va_end(ap);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp)
    void warn(const char* fmt, ...) const
    {
        if (this->disable_output_)
        {
            return;
        }

        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "[WARN] ");
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp)
    void error(const char* fmt, ...) const
    {
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "[ERROR] ");
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp)
    void print(const char* fmt, ...) const
    {
        if (this->disable_output_)
        {
            return;
        }

        va_list ap;
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

#pragma once
#include "generic_logger.hpp"

class logger : public generic_logger
{
  public:
    void print(color c, std::string_view message) override;
    void print(color c, const char* message, ...) override FORMAT_ATTRIBUTE(3, 4);
    void info(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void warn(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void error(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void success(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void log(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void newline();

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
    void print_message(color c, std::string_view message, bool force = false) const;
};

#pragma once

#ifdef OS_WINDOWS
#define FORMAT_ATTRIBUTE(fmt_pos, var_pos)
#else
#define FORMAT_ATTRIBUTE(fmt_pos, var_pos) __attribute__((format(printf, fmt_pos, var_pos)))
#endif

enum class color
{
    black,
    red,
    green,
    yellow,
    blue,
    cyan,
    pink,
    white,
    gray,
    dark_gray,
};

class logger
{
  public:
    void print(color c, std::string_view message) const;
    void print(color c, const char* message, ...) const FORMAT_ATTRIBUTE(3, 4);
    void info(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void warn(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void error(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void success(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void log(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);

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

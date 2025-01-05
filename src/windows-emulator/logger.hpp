#pragma once

#ifdef OS_WINDOWS
#define FORMAT_ATTRIBUTE(fmt_pos, var_pos)
#else
#define FORMAT_ATTRIBUTE(fmt_pos, var_pos) __attribute__((format( printf, fmt_pos, var_pos)))
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
	void print(color c, const char* message, ...) const FORMAT_ATTRIBUTE(3, 4);

	template <typename... Args>
	void info(const char* message, Args... args) const
	{
		this->print(color::cyan, message, args...);
	}

	template <typename... Args>
	void warn(const char* message, Args... args) const
	{
		this->print(color::yellow, message, args...);
	}

	template <typename... Args>
	void error(const char* message, Args... args) const
	{
		this->print(color::red, message, args...);
	}

	template <typename... Args>
	void success(const char* message, Args... args) const
	{
		this->print(color::green, message, args...);
	}

	template <typename... Args>
	void log(const char* message, Args... args) const
	{
		this->print(color::gray, message, args...);
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

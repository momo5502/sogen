#pragma once

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
	void print(color c, const char* message, ...) const;

	template <typename... Args>
	void info(const char* message, Args... args)
	{
		this->print(color::cyan, message, args...);
	}

	template <typename... Args>
	void warn(const char* message, Args... args)
	{
		this->print(color::yellow, message, args...);
	}

	template <typename... Args>
	void error(const char* message, Args... args)
	{
		this->print(color::red, message, args...);
	}

	template <typename... Args>
	void success(const char* message, Args... args)
	{
		this->print(color::green, message, args...);
	}

	template <typename... Args>
	void log(const char* message, Args... args)
	{
		this->print(color::gray, message, args...);
	}

	void disable_output(const bool value)
	{
		this->disable_output_ = value;
	}

private:
	bool disable_output_{false};
};

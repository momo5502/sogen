#pragma once

struct unicorn_error : std::runtime_error
{
	unicorn_error(const uc_err error_code)
		: std::runtime_error(uc_strerror(error_code))
		  , code(error_code)
	{
	}

	uc_err code{};
};

inline void ThrowIfUnicornError(const uc_err error_code)
{
	if (error_code != UC_ERR_OK)
	{
		throw unicorn_error(error_code);
	}
}

#define e ThrowIfUnicornError

class unicorn
{
public:
	unicorn() = default;
	unicorn(uc_arch arch, uc_mode mode);

	unicorn(const unicorn& obj) = delete;
	unicorn& operator=(const unicorn& obj) = delete;

	unicorn(unicorn&& obj) noexcept;
	unicorn& operator=(unicorn&& obj) noexcept;

	~unicorn();

	void close();

	operator uc_engine*() const
	{
		return this->uc_;
	}

private:
	uc_engine* uc_{};
};
